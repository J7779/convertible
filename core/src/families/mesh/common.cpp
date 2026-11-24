// Convertible core - mesh family: shared reader/writer helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/common.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include "engine/standard_options.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

// --------------------------------------------------------------------------- limits

std::uint64_t max_elements(const Context& ctx) noexcept {
  const std::uint64_t by_memory = ctx.limits.max_decompressed_bytes / 16;
  return std::max<std::uint64_t>(1024, std::min<std::uint64_t>(by_memory, 1ull << 31));
}

Result<void> check_elements(const Context& ctx, std::uint64_t count, std::string_view what) {
  if (count > max_elements(ctx)) {
    return fail(Status::Limit, "{} {} exceed the limit of {}", count, what, max_elements(ctx));
  }
  return {};
}

Result<void> check_records(const Context& ctx, std::uint64_t count, std::uint64_t bytes_each, std::uint64_t available,
                           std::string_view what) {
  if (bytes_each != 0 && count > available / bytes_each) {
    return fail(Status::Decode, "the file is truncated: {} {} declared but only {} bytes remain", count, what,
                available);
  }
  return check_elements(ctx, count, what);
}

// --------------------------------------------------------------------------- text parsing

LineReader::LineReader(std::string_view text) noexcept : text_(text) {
  if (text_.starts_with("\xEF\xBB\xBF")) pos_ = 3;
}

bool LineReader::next(std::string_view& line) noexcept {
  if (pos_ >= text_.size()) return false;
  const std::size_t start = pos_;
  std::size_t end = start;
  while (end < text_.size() && text_[end] != '\n' && text_[end] != '\r') ++end;
  line = text_.substr(start, end - start);
  pos_ = end;
  if (pos_ < text_.size() && text_[pos_] == '\r') ++pos_;
  if (pos_ < text_.size() && text_[pos_] == '\n' && (pos_ == end || text_[pos_ - 1] == '\r')) ++pos_;
  ++line_;
  return true;
}

std::string_view next_token(std::string_view& text) noexcept {
  std::size_t i = 0;
  while (i < text.size() && util::is_ascii_space(text[i])) ++i;
  std::size_t j = i;
  while (j < text.size() && !util::is_ascii_space(text[j])) ++j;
  const std::string_view token = text.substr(i, j - i);
  text.remove_prefix(j);
  return token;
}

std::vector<std::string_view> tokens(std::string_view text, std::string_view extra) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  const auto separator = [&](char c) { return util::is_ascii_space(c) || extra.find(c) != std::string_view::npos; };
  while (i < text.size()) {
    while (i < text.size() && separator(text[i])) ++i;
    std::size_t j = i;
    while (j < text.size() && !separator(text[j])) ++j;
    if (j > i) out.push_back(text.substr(i, j - i));
    i = j;
  }
  return out;
}

namespace {

// Exact fast path (Clinger): up to 19 digits whose value fits 2^53 and a
// power of ten up to 22 are converted with one correctly rounded multiply
// or divide. Everything else goes to the general (slower) parser.
bool fast_double(std::string_view s, double& out) noexcept {
  std::size_t i = 0;
  bool negative = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
  std::uint64_t mantissa = 0;
  int digits = 0;
  int exponent = 0;
  bool any = false;
  while (i < s.size() && util::is_ascii_digit(s[i])) {
    any = true;
    if (digits < 19) {
      mantissa = mantissa * 10 + static_cast<std::uint64_t>(s[i] - '0');
      if (mantissa != 0) ++digits;
    } else {
      ++exponent;
      if (s[i] != '0') return false;
    }
    ++i;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && util::is_ascii_digit(s[i])) {
      any = true;
      if (digits < 19) {
        mantissa = mantissa * 10 + static_cast<std::uint64_t>(s[i] - '0');
        if (mantissa != 0) ++digits;
        --exponent;
      } else if (s[i] != '0') {
        return false;
      }
      ++i;
    }
  }
  if (!any) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool exp_negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) exp_negative = s[i++] == '-';
    int e = 0;
    bool exp_digits = false;
    while (i < s.size() && util::is_ascii_digit(s[i])) {
      exp_digits = true;
      if (e < 100000) e = e * 10 + (s[i] - '0');
      ++i;
    }
    if (!exp_digits) return false;
    exponent += exp_negative ? -e : e;
  }
  if (i != s.size()) return false;
  if (mantissa > (1ull << 53) || exponent < -22 || exponent > 22) return false;
  static constexpr double powers[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  double value = static_cast<double>(mantissa);
  value = exponent < 0 ? value / powers[-exponent] : value * powers[exponent];
  out = negative ? -value : value;
  return true;
}

}  // namespace

bool parse_double(std::string_view token, double& out) noexcept {
  if (token.empty()) return false;
  if (fast_double(token, out)) return true;
  const auto parsed = util::parse_double(token);
  if (!parsed) return false;
  out = *parsed;
  return true;
}

bool parse_float(std::string_view token, float& out) noexcept {
  double value = 0;
  if (!parse_double(token, value)) return false;
  if (std::isfinite(value) && std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
    out = value < 0 ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
  } else {
    out = static_cast<float>(value);
  }
  return true;
}

bool parse_int(std::string_view token, std::int64_t& out) noexcept {
  if (!token.empty() && token.front() == '+') token.remove_prefix(1);
  if (token.empty()) return false;
  const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), out);
  return ec == std::errc{} && ptr == token.data() + token.size();
}

std::string_view NumberScanner::next_raw() noexcept {
  const auto separator = [this](char c) { return util::is_ascii_space(c) || (commas_ && c == ','); };
  while (pos_ < text_.size() && separator(text_[pos_])) ++pos_;
  const std::size_t start = pos_;
  while (pos_ < text_.size() && !separator(text_[pos_])) ++pos_;
  return text_.substr(start, pos_ - start);
}

bool NumberScanner::next(double& out) noexcept {
  const std::size_t before = pos_;
  const std::string_view token = next_raw();
  if (token.empty()) return false;
  if (!parse_double(token, out)) {
    bad_ = true;
    pos_ = before;
    return false;
  }
  return true;
}

bool NumberScanner::next_float(float& out) noexcept {
  double value = 0;
  if (!next(value)) return false;
  out = static_cast<float>(value);
  return true;
}

bool NumberScanner::next_int(std::int64_t& out) noexcept {
  const std::size_t before = pos_;
  const std::string_view token = next_raw();
  if (token.empty()) return false;
  if (!parse_int(token, out)) {
    double value = 0;  // tolerate "3.0"
    if (parse_double(token, value) && std::isfinite(value) && value == std::floor(value) && std::abs(value) < 9e15) {
      out = static_cast<std::int64_t>(value);
      return true;
    }
    bad_ = true;
    pos_ = before;
    return false;
  }
  return true;
}

Result<std::vector<float>> parse_float_list(const Context& ctx, std::string_view text, std::string_view what) {
  std::vector<float> values;
  values.reserve(std::min<std::size_t>(text.size() / 2 + 1, 1u << 20));
  NumberScanner scanner(text);
  float value = 0;
  while (scanner.next_float(value)) {
    values.push_back(value);
    if ((values.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx, values.size(), what));
  }
  if (scanner.bad()) return fail(Status::Decode, "{} contain a value that is not a number", what);
  return values;
}

Result<std::vector<std::int64_t>> parse_int_list(const Context& ctx, std::string_view text, std::string_view what) {
  std::vector<std::int64_t> values;
  values.reserve(std::min<std::size_t>(text.size() / 2 + 1, 1u << 20));
  NumberScanner scanner(text);
  std::int64_t value = 0;
  while (scanner.next_int(value)) {
    values.push_back(value);
    if ((values.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx, values.size(), what));
  }
  if (scanner.bad()) return fail(Status::Decode, "{} contain a value that is not an integer", what);
  return values;
}

// --------------------------------------------------------------------------- text output

void append_number(std::string& out, double value, int precision) {
  if (!std::isfinite(value)) {
    out += std::isnan(value) ? "0" : (value < 0 ? "-1e308" : "1e308");
    return;
  }
  if (value == 0.0) value = 0.0;  // no "-0"
  std::array<char, 64> buffer{};
  std::to_chars_result result{};
  if (precision <= 0) {
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  } else {
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general,
                           std::min(precision, 17));
  }
  if (result.ec != std::errc{}) {
    out += '0';
    return;
  }
  std::string_view text(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
  if (text == "-0") text = "0";
  out.append(text);
}

void append_number(std::string& out, float value, int precision) {
  if (!std::isfinite(value)) {
    append_number(out, static_cast<double>(value), precision);
    return;
  }
  if (value == 0.0f) value = 0.0f;
  std::array<char, 64> buffer{};
  std::to_chars_result result{};
  if (precision <= 0) {
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  } else {
    result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general,
                           std::min(precision, 9));
  }
  if (result.ec != std::errc{}) {
    out += '0';
    return;
  }
  std::string_view text(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
  if (text == "-0") text = "0";
  out.append(text);
}

std::string format_number(double value, int precision) {
  std::string out;
  append_number(out, value, precision);
  return out;
}

// --------------------------------------------------------------------------- colours

float srgb_to_linear(float value) noexcept {
  if (!std::isfinite(value)) return 0.0f;
  const double v = std::clamp(static_cast<double>(value), 0.0, 1.0);
  return static_cast<float>(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
}

float linear_to_srgb(float value) noexcept {
  if (!std::isfinite(value)) return 0.0f;
  const double v = std::clamp(static_cast<double>(value), 0.0, 1.0);
  return static_cast<float>(v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
}

ir::Color color_from_srgb(double r, double g, double b, double a, double max) noexcept {
  const auto unit = [max](double v) { return static_cast<float>(std::clamp(v / max, 0.0, 1.0)); };
  return {srgb_to_linear(unit(r)), srgb_to_linear(unit(g)), srgb_to_linear(unit(b)), a < 0.0 ? 1.0f : unit(a)};
}

std::array<std::uint8_t, 4> color_to_srgb8(const ir::Color& color) noexcept {
  const auto byte = [](float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 255.0f));
  };
  return {byte(linear_to_srgb(color.r)), byte(linear_to_srgb(color.g)), byte(linear_to_srgb(color.b)),
          byte(color.a)};
}

// --------------------------------------------------------------------------- reading

Result<void> finish_read(ir::Mesh& mesh, const ReadContext& ctx) {
  const std::size_t n = mesh.positions.size();
  if (n == 0) return fail(Status::Decode, "the file contains no geometry");
  const auto check_array = [&](auto& values, std::string_view what) {
    if (!values.empty() && values.size() != n) {
      ctx.warn(std::format("Ignored {} {}: the file has {} vertices.", values.size(), what, n));
      values.clear();
    }
  };
  check_array(mesh.normals, "normals");
  check_array(mesh.texcoords, "texture coordinates");
  check_array(mesh.colors, "vertex colours");
  std::erase_if(mesh.attributes, [&](const ir::VertexAttribute& a) { return a.values.size() != n; });
  std::size_t bad_positions = 0;
  for (ir::Vec3& p : mesh.positions) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
      p = {0, 0, 0};
      ++bad_positions;
    }
  }
  if (bad_positions > 0) ctx.warn(std::format("{} vertices had invalid coordinates and were set to 0.", bad_positions));
  if (!mesh.face_sizes.empty()) {
    std::uint64_t total = 0;
    for (const std::uint32_t size : mesh.face_sizes) {
      if (size < 3) return fail(Status::Decode, "a face has fewer than three corners");
      total += size;
    }
    if (total != mesh.indices.size()) return fail(Status::Decode, "face sizes do not match the index count");
    if (mesh.is_triangulated()) mesh.face_sizes.clear();
  } else if (mesh.indices.size() % 3 != 0) {
    return fail(Status::Decode, "the triangle list is incomplete");
  }
  for (const std::uint32_t index : mesh.indices) {
    if (index >= n) return fail(Status::Decode, "a face refers to vertex {} but there are only {}", index, n);
  }
  normalize_normals(mesh);
  for (ir::Color& c : mesh.colors) {
    const auto clamp01 = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
    c = {clamp01(c.r), clamp01(c.g), clamp01(c.b), clamp01(c.a)};
  }
  const std::size_t faces = mesh.face_count();
  const auto clip = [faces](std::size_t& first, std::size_t& count) {
    first = std::min(first, faces);
    count = std::min(count, faces - first);
  };
  for (ir::MeshGroup& group : mesh.groups) clip(group.first_face, group.face_count);
  std::erase_if(mesh.groups, [](const ir::MeshGroup& g) { return g.face_count == 0; });
  for (ir::MeshObject& object : mesh.objects) clip(object.first_face, object.face_count);
  std::erase_if(mesh.objects, [](const ir::MeshObject& o) { return o.face_count == 0; });
  for (ir::Material& material : mesh.materials) {
    if (material.base_color_texture >= static_cast<int>(mesh.textures.size())) material.base_color_texture = -1;
    if (material.normal_texture >= static_cast<int>(mesh.textures.size())) material.normal_texture = -1;
  }
  if (mesh.indices.empty()) {
    mesh.point_cloud = true;
    mesh.face_sizes.clear();
    mesh.groups.clear();
    mesh.objects.clear();
  }
  if (!mesh.valid()) return fail(Status::Decode, "the mesh is inconsistent");
  return {};
}

// --------------------------------------------------------------------------- writing

std::vector<OptionSpec> mesh_write_options(const WriterTraits& traits) {
  std::vector<OptionSpec> specs;
  if (!traits.points_only) {
    specs.push_back(options::quality());
    specs.push_back(options::preset());
  }
  if (traits.normals) {
    specs.push_back(OptionSpec::boolean("include_normals", "Include normals", true,
                                        "Write vertex normals when the model has them."));
  }
  if (traits.texcoords) {
    specs.push_back(OptionSpec::boolean("include_uvs", "Include texture coordinates", true,
                                        "Write texture coordinates when the model has them."));
  }
  if (traits.colors) {
    specs.push_back(
        OptionSpec::boolean("include_colors", "Include vertex colours", true, "Write per-vertex colours when present."));
  }
  if (!traits.triangles_only) {
    specs.push_back(OptionSpec::boolean("triangulate", "Triangulate", false,
                                        "Split polygons into triangles before writing."));
  }
  specs.push_back(OptionSpec::choice("up_axis", "Up axis", "auto", {"auto", "y", "z"},
                                     "Axis that points up in the output; auto follows the format's convention.")
                      .advanced());
  if (traits.objects) {
    specs.push_back(OptionSpec::boolean("merge_objects", "Merge objects", false,
                                        "Write the whole scene as one object instead of separate ones."));
  }
  specs.push_back(OptionSpec::boolean("optimize", "Optimise", true,
                                      "Merge identical vertices and drop unused vertices and degenerate faces.")
                      .advanced());
  if (traits.text) {
    specs.push_back(OptionSpec::integer("precision", "Number precision", 0, 0, 17,
                                        "Significant digits of coordinates; 0 writes the shortest exact form.")
                        .advanced());
  }
  specs.push_back(options::strip_metadata());
  return specs;
}

int write_precision(const Context& ctx) { return static_cast<int>(ctx.options.get_int("precision", 0, 0, 17)); }

Result<ir::Mesh> prepare_for_write(const ir::Mesh& source, WriteContext& ctx, const WriterTraits& traits) {
  if (source.positions.empty()) return fail(Status::Encode, "the model has no geometry");
  if (!source.valid()) return fail(Status::Encode, "the model is inconsistent");
  if (source.point_cloud && !traits.point_clouds && !traits.points_only) {
    return fail(Status::Encode, "this format cannot hold a point cloud (a model without faces)");
  }
  ir::Mesh mesh = source;
  if (options::resolved_strip_metadata(ctx.options)) mesh.metadata.clear();
  if (ctx.options.get_bool("merge_objects", false)) mesh.objects.clear();

  if (traits.points_only) {
    mesh.indices.clear();
    mesh.face_sizes.clear();
    mesh.groups.clear();
    mesh.objects.clear();
    mesh.point_cloud = true;
  }
  if (!mesh.point_cloud && !mesh.indices.empty()) {
    const int quality = options::resolved_quality(ctx.options);
    const float ratio = quality_to_ratio(quality);
    if (ratio < 1.0f) {
      // Per-face (flat) normals split every corner, which would lock the
      // whole surface; normals are dropped first and rebuilt afterwards
      // with a crease angle, so hard edges stay hard.
      const bool had_normals = !mesh.normals.empty();
      if (had_normals) {
        mesh.normals.clear();
        weld(mesh, 0.0f, false, 0.0f);
      }
      SimplifyOptions simplify_options;
      simplify_options.target_ratio = ratio;
      const bool finished =
          simplify(mesh, simplify_options, [&ctx](double fraction) { return ctx.checkpoint(fraction * 0.8, "Simplifying").has_value(); });
      if (!finished) return fail(Status::Cancelled, "the conversion was cancelled");
      if (had_normals) generate_normals(mesh, 45.0f);
    }
    if (ctx.options.get_bool("optimize", true)) {
      weld(mesh, 0.0f, false, 0.0f);
      remove_degenerate_faces(mesh, true, 0.0f);
      remove_unused_vertices(mesh);
    }
    if (traits.triangles_only || ctx.options.get_bool("triangulate", false)) triangulate(mesh);
  }
  if (!traits.normals || !ctx.options.get_bool("include_normals", true)) mesh.normals.clear();
  if (!traits.texcoords || !ctx.options.get_bool("include_uvs", true)) mesh.texcoords.clear();
  if (!traits.colors || !ctx.options.get_bool("include_colors", true)) mesh.colors.clear();

  const std::string up = ctx.options.get_enum("up_axis", "auto", {"auto", "y", "z"});
  bool to_z = false;
  if (up == "z") {
    to_z = true;
  } else if (up == "auto") {
    switch (traits.up) {
      case WriterTraits::Up::Z: to_z = !mesh.source_up_axis.empty(); break;
      case WriterTraits::Up::Agnostic: to_z = mesh.source_up_axis == "Z"; break;
      case WriterTraits::Up::Y: to_z = false; break;
    }
  }
  if (to_z) convert_up_axis(mesh, UpAxis::Y, UpAxis::Z);
  normalize_normals(mesh);
  if (mesh.positions.empty()) return fail(Status::Encode, "nothing is left of the model after simplification");
  return mesh;
}

OptionSpec unit_option() {
  return OptionSpec::choice("unit", "Unit", "auto", {"auto", "m", "cm", "mm", "in", "ft"},
                            "Length unit declared in the file; auto keeps the model's own unit.")
      .advanced();
}

UnitChoice resolve_unit(const ir::Mesh& mesh, const Context& ctx, double format_default) {
  const std::string unit = ctx.options.get_enum("unit", "auto", {"auto", "m", "cm", "mm", "in", "ft"});
  UnitChoice choice;
  if (unit == "auto") {
    choice.meters = mesh.unit_meters > 0.0 ? mesh.unit_meters : format_default;
    return choice;
  }
  double meters = 1.0;
  if (unit == "cm") meters = 0.01;
  else if (unit == "mm") meters = 0.001;
  else if (unit == "in") meters = 0.0254;
  else if (unit == "ft") meters = 0.3048;
  choice.meters = meters;
  if (mesh.unit_meters > 0.0) choice.scale = static_cast<float>(mesh.unit_meters / meters);
  return choice;
}

// --------------------------------------------------------------------------- textures

std::string image_mime(ByteView data, std::string_view name) {
  const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 16)));
  if (head.starts_with("\x89PNG")) return "image/png";
  if (head.starts_with("\xFF\xD8\xFF")) return "image/jpeg";
  if (head.starts_with("GIF8")) return "image/gif";
  if (head.starts_with("BM")) return "image/bmp";
  if (head.size() >= 12 && head.starts_with("RIFF") && head.substr(8, 4) == "WEBP") return "image/webp";
  if (head.starts_with("II*") || head.starts_with("MM\0*")) return "image/tiff";
  if (head.starts_with("DDS ")) return "image/vnd-ms.dds";
  if (head.size() >= 12 && head.substr(4, 8) == "ftypavif") return "image/avif";
  if (head.starts_with("\xABKTX 20")) return "image/ktx2";
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) return {};
  const std::string ext = util::to_lower(name.substr(dot + 1));
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "bmp") return "image/bmp";
  if (ext == "tga") return "image/x-tga";
  if (ext == "tif" || ext == "tiff") return "image/tiff";
  if (ext == "webp") return "image/webp";
  if (ext == "dds") return "image/vnd-ms.dds";
  return {};
}

std::string_view image_extension(std::string_view mime) noexcept {
  if (mime == "image/png") return "png";
  if (mime == "image/jpeg") return "jpg";
  if (mime == "image/gif") return "gif";
  if (mime == "image/bmp") return "bmp";
  if (mime == "image/x-tga") return "tga";
  if (mime == "image/tiff") return "tif";
  if (mime == "image/webp") return "webp";
  if (mime == "image/vnd-ms.dds") return "dds";
  if (mime == "image/avif") return "avif";
  if (mime == "image/ktx2") return "ktx2";
  return "bin";
}

std::string_view base_name(std::string_view path) noexcept {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string resolve_relative(std::string_view base, std::string_view reference) {
  std::string ref(reference);
  for (char& c : ref) {
    if (c == '\\') c = '/';
  }
  std::vector<std::string> parts;
  const std::size_t slash = base.find_last_of('/');
  if (slash != std::string_view::npos && !ref.starts_with('/')) {
    for (const std::string_view piece : util::split(base.substr(0, slash), '/', false)) parts.emplace_back(piece);
  }
  for (const std::string_view piece : util::split(ref, '/', false)) {
    if (piece == ".") continue;
    if (piece == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.emplace_back(piece);
  }
  return util::join(parts, "/");
}

// --------------------------------------------------------------------------- sniffing helpers

std::string_view text_head(ByteView data, std::size_t limit) noexcept {
  std::string_view text = as_chars(data.first(std::min(data.size(), limit)));
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return text;
}

bool looks_textual(ByteView data) noexcept {
  const std::string_view head = text_head(data, 1024);
  if (head.empty()) return false;
  std::size_t odd = 0;
  for (const char c : head) {
    const auto u = static_cast<unsigned char>(c);
    if (u == 0) return false;
    if (u < 0x09 || (u > 0x0D && u < 0x20)) ++odd;
  }
  return odd * 50 < head.size();
}

bool has_extension(std::string_view filename, std::string_view extension) noexcept {
  const std::size_t dot = filename.rfind('.');
  const std::string_view ext = dot == std::string_view::npos ? filename : filename.substr(dot + 1);
  return util::iequals(ext, extension);
}

}  // namespace cvt::mesh
