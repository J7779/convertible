// Convertible core - mesh family: shared reader/writer helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/mesh.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
struct FormatInfo;
}  // namespace cvt

namespace cvt::mesh {

// --------------------------------------------------------------------------- registration (one per source file group)

void register_classic_formats(Registry& registry);      // off, obj (+ obj.zip), stl, ply
void register_point_cloud_formats(Registry& registry);  // xyz/pts, pcd, las, vtk, msh
void register_scene_formats(Registry& registry);        // gltf, glb, 3mf, amf
void register_usd_x3d_formats(Registry& registry);      // usda, usdz, x3d, wrl
void register_legacy_formats(Registry& registry);       // dae, fbx
void register_game_formats(Registry& registry);         // 3ds, lwo, md2, md3, ms3d, x, vox
void register_mesh_bridges(Registry& registry);         // Mesh -> Image / Data / Table

// --------------------------------------------------------------------------- limits

/// Largest number of vertices, indices or points a reader may allocate,
/// derived from the conversion's decompression ceiling (16 bytes each).
[[nodiscard]] std::uint64_t max_elements(const Context& ctx) noexcept;
/// Status::Limit when `count` elements of `what` exceed max_elements().
[[nodiscard]] Result<void> check_elements(const Context& ctx, std::uint64_t count, std::string_view what);
/// For binary formats: `count` records of `bytes_each` must fit in the
/// `available` bytes that remain (Status::Decode "truncated" otherwise), and
/// pass check_elements.
[[nodiscard]] Result<void> check_records(const Context& ctx, std::uint64_t count, std::uint64_t bytes_each,
                                         std::uint64_t available, std::string_view what);

// --------------------------------------------------------------------------- text parsing

/// Splits text into lines (LF, CRLF or CR) without copying. A UTF-8 BOM at
/// the start is skipped.
class LineReader {
 public:
  explicit LineReader(std::string_view text) noexcept;
  /// False at the end of the text.
  bool next(std::string_view& line) noexcept;
  /// 1-based number of the line last returned.
  [[nodiscard]] std::size_t line_number() const noexcept { return line_; }
  /// Byte offset of the next unread character.
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
  std::size_t line_ = 0;
};

/// Next whitespace-separated token of `text` (which is advanced past it);
/// empty when none is left.
[[nodiscard]] std::string_view next_token(std::string_view& text) noexcept;
/// Split on ASCII white space (and optionally on `extra` separators such as ',').
[[nodiscard]] std::vector<std::string_view> tokens(std::string_view text, std::string_view extra = {});

/// Locale-free number parsing of a whole token.
[[nodiscard]] bool parse_float(std::string_view token, float& out) noexcept;
[[nodiscard]] bool parse_double(std::string_view token, double& out) noexcept;
[[nodiscard]] bool parse_int(std::string_view token, std::int64_t& out) noexcept;

/// Streams whitespace-separated numbers out of a text blob (COLLADA
/// arrays, X3D attributes, VRML lists, ...). Commas count as separators
/// when `commas` is set.
class NumberScanner {
 public:
  explicit NumberScanner(std::string_view text, bool commas = true) noexcept : text_(text), commas_(commas) {}
  /// False at the end or on a token that is not a number (see `bad()`).
  bool next(double& out) noexcept;
  bool next_float(float& out) noexcept;
  bool next_int(std::int64_t& out) noexcept;
  [[nodiscard]] bool bad() const noexcept { return bad_; }
  [[nodiscard]] std::string_view rest() const noexcept { return text_.substr(pos_); }

 private:
  std::string_view next_raw() noexcept;
  std::string_view text_;
  std::size_t pos_ = 0;
  bool commas_ = true;
  bool bad_ = false;
};

/// Read every number of `text` into floats; Status::Decode on garbage,
/// Status::Limit via check_elements.
[[nodiscard]] Result<std::vector<float>> parse_float_list(const Context& ctx, std::string_view text,
                                                          std::string_view what);
[[nodiscard]] Result<std::vector<std::int64_t>> parse_int_list(const Context& ctx, std::string_view text,
                                                               std::string_view what);

/// A number read from a file as a count or index: negative, non-finite and
/// absurdly large values (>= 2^53) become `invalid_index`, so range checks
/// written in 64-bit arithmetic reject them instead of hitting undefined
/// float-to-integer conversions.
inline constexpr std::uint64_t invalid_index = ~0ull;
[[nodiscard]] constexpr std::uint64_t file_index(double value) noexcept {
  return value >= 0.0 && value < 9007199254740992.0 ? static_cast<std::uint64_t>(value) : invalid_index;
}

// --------------------------------------------------------------------------- text output

/// Append a number, locale-free: `precision` 0 = shortest representation
/// that reads back exactly; otherwise that many significant digits with
/// trailing zeros trimmed. "-0" is written as "0".
void append_number(std::string& out, double value, int precision = 0);
void append_number(std::string& out, float value, int precision = 0);
[[nodiscard]] std::string format_number(double value, int precision = 0);

// --------------------------------------------------------------------------- colours

// ir::Color is LINEAR. Colours a format stores as 8/16-bit integers (PLY,
// OFF, STL, PCD, LAS, 3MF "#RRGGBB", VOX palettes, ...) are sRGB-encoded
// display values and go through these helpers; float colours of formats
// that define them as linear (glTF, USD, FBX, COLLADA, MTL, X3D/VRML)
// are used as they are. Alpha is never transfer-encoded.
[[nodiscard]] float srgb_to_linear(float value) noexcept;
[[nodiscard]] float linear_to_srgb(float value) noexcept;
/// From sRGB components scaled to `max` (255 for bytes, 65535 for 16 bits).
[[nodiscard]] ir::Color color_from_srgb(double r, double g, double b, double a = -1.0, double max = 255.0) noexcept;
/// To sRGB bytes R, G, B, A.
[[nodiscard]] std::array<std::uint8_t, 4> color_to_srgb8(const ir::Color& color) noexcept;

// --------------------------------------------------------------------------- reading

/// Final checks every reader applies before returning: drops per-vertex
/// arrays of the wrong length (with a warning), validates indices (Decode
/// error when out of range), normalises normals, and clears degenerate
/// empty groups/objects.
[[nodiscard]] Result<void> finish_read(ir::Mesh& mesh, const ReadContext& ctx);

// --------------------------------------------------------------------------- writing

/// What a writer's format can hold; drives `prepare_for_write`.
struct WriterTraits {
  /// Only triangles (STL, glTF, 3DS, ...): polygons are triangulated.
  bool triangles_only = false;
  bool normals = true;
  bool texcoords = true;
  bool colors = true;
  bool materials = true;
  /// Separate objects/nodes with transforms.
  bool objects = false;
  /// Points without faces.
  bool point_clouds = false;
  /// The format stores only points (xyz, pcd, las): faces are ignored, the
  /// vertices of a mesh are written as a point cloud.
  bool points_only = false;
  /// Text format: the `precision` option applies.
  bool text = false;
  /// The format's up-axis convention.
  enum class Up { Agnostic, Y, Z } up = Up::Agnostic;
};

/// The writer options every mesh writer shares, filtered by `traits`:
/// quality + preset (simplification), strip_metadata, include_normals,
/// include_uvs, include_colors, triangulate, up_axis, merge_objects,
/// optimize, precision (text formats).
[[nodiscard]] std::vector<OptionSpec> mesh_write_options(const WriterTraits& traits);

/// A copy of `source` ready to be written: metadata stripped when asked,
/// objects merged when asked or unsupported, simplified when `quality` (or
/// the preset) is below 90, cleaned (exact duplicates welded, unused
/// vertices and degenerate faces dropped) when `optimize` is on,
/// triangulated when asked or required, optional arrays dropped by option
/// or capability, and rotated into the format's axis convention:
///   - `up_axis` "y" / "z": rotate from the IR's +Y up when "z";
///   - "auto": Z-up formats (3MF, AMF, 3DS, LAS) rotate unless the source
///     declared no axis; axis-agnostic formats (STL, PLY, OFF, VTK, point
///     lists) restore a declared Z-up source; Y-up formats (glTF, OBJ by
///     convention, FBX, COLLADA, USD, X3D) keep the IR as is.
/// Readers of axis-agnostic formats leave `source_up_axis` empty: their
/// coordinates are taken as they are, and a Z-up writer then treats them as
/// already Z-up (STL -> 3MF keeps a printable orientation).
/// Point clouds pass through untouched for formats with `point_clouds`;
/// others fail with Status::Encode.
[[nodiscard]] Result<ir::Mesh> prepare_for_write(const ir::Mesh& source, WriteContext& ctx,
                                                 const WriterTraits& traits);

/// Text precision chosen by the `precision` option (0 = exact shortest).
[[nodiscard]] int write_precision(const Context& ctx);

/// A unit name ("millimeter", "meter", ...) for writers that declare
/// units; `meters` 0 maps to `fallback`.
struct UnitChoice {
  double meters = 0.0;     ///< size of the unit that will be declared
  float scale = 1.0f;      ///< factor to apply to the coordinates first
};
/// Reads the `unit` option ("auto", "m", "cm", "mm", "in", "ft"): with
/// "auto" the mesh's own unit is declared (or `format_default` when it is
/// unknown, without scaling); an explicit unit rescales known units.
[[nodiscard]] UnitChoice resolve_unit(const ir::Mesh& mesh, const Context& ctx, double format_default);
/// The `unit` OptionSpec for writers that declare units.
[[nodiscard]] OptionSpec unit_option();

// --------------------------------------------------------------------------- textures

/// Media type of an encoded image from its magic number, else from the
/// file name's extension; empty when unknown.
[[nodiscard]] std::string image_mime(ByteView data, std::string_view name = {});
/// A file extension (without dot) for a media type ("png" for image/png).
[[nodiscard]] std::string_view image_extension(std::string_view mime) noexcept;
/// The last path component of a reference ("textures/a.png" -> "a.png").
[[nodiscard]] std::string_view base_name(std::string_view path) noexcept;
/// Join a relative reference to the directory of `base` ("dir/m.mtl" +
/// "t.png" -> "dir/t.png"), resolving "./" and "../" segments.
[[nodiscard]] std::string resolve_relative(std::string_view base, std::string_view reference);

// --------------------------------------------------------------------------- sniffing helpers

/// The first `limit` bytes as text, without a UTF-8 BOM.
[[nodiscard]] std::string_view text_head(ByteView data, std::size_t limit = 4096) noexcept;
/// True when the head of `data` is plausible text (no NULs, mostly printable).
[[nodiscard]] bool looks_textual(ByteView data) noexcept;
/// Case-insensitive extension test on a file name ("model.OBJ", "obj").
[[nodiscard]] bool has_extension(std::string_view filename, std::string_view extension) noexcept;

}  // namespace cvt::mesh
