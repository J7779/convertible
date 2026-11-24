// Convertible core - mesh family: Object File Format (OFF, COFF, NOFF, STOFF, ...).
//
// Text variant of the Geomview format: an optional header keyword made of
// the prefixes ST (texture coordinates), C (vertex colours), N (normals),
// 4 (homogeneous coordinates) and n (dimension given) before "OFF", then
// "vertices faces edges", the vertex lines and one line per face:
// "n i0 i1 ... [r g b [a]]". Colours are 0..1 floats or 0..255 integers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/mesh/common.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

struct OffHeader {
  bool texcoords = false;
  bool colors = false;
  bool normals = false;
  bool homogeneous = false;
  bool dimension = false;
};

// Parses a header keyword such as "STCNOFF"; false when it is not one.
bool parse_off_keyword(std::string_view word, OffHeader& header) {
  if (!word.ends_with("OFF")) return false;
  word.remove_suffix(3);
  // Writers disagree on the order of the prefixes ("CNOFF", "NCOFF"); each
  // may appear once.
  while (!word.empty()) {
    bool* flag = nullptr;
    std::size_t length = 1;
    if (word.starts_with("ST")) {
      flag = &header.texcoords;
      length = 2;
    } else if (word.front() == 'C') {
      flag = &header.colors;
    } else if (word.front() == 'N') {
      flag = &header.normals;
    } else if (word.front() == '4') {
      flag = &header.homogeneous;
    } else if (word.front() == 'n') {
      flag = &header.dimension;
    }
    if (flag == nullptr || *flag) return false;
    *flag = true;
    word.remove_prefix(length);
  }
  return true;
}

// Token stream over the whole file that skips '#' comments.
class OffTokens {
 public:
  explicit OffTokens(std::string_view text) : lines_(text) {}
  std::string_view next() {
    for (;;) {
      std::string_view token = next_token(line_);
      if (!token.empty()) return token;
      std::string_view raw;
      if (!lines_.next(raw)) return {};
      const std::size_t hash = raw.find('#');
      line_ = hash == std::string_view::npos ? raw : raw.substr(0, hash);
    }
  }
  /// Remaining tokens of the current line (the optional face colour).
  std::vector<std::string_view> rest_of_line() {
    std::vector<std::string_view> out;
    for (std::string_view token = next_token(line_); !token.empty(); token = next_token(line_)) out.push_back(token);
    return out;
  }

 private:
  LineReader lines_;
  std::string_view line_;
};

Result<AnyIR> read_off(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  // The counts may follow the keyword on the same line.
  OffTokens counts_in(text);
  OffHeader header;
  if (!parse_off_keyword(counts_in.next(), header)) {
    return fail(Status::Decode, "not an OFF file (missing the OFF keyword)");
  }
  if (header.homogeneous) return fail(Status::Decode, "homogeneous (4OFF) coordinates are not supported");
  std::string_view first = counts_in.next();
  if (util::iequals(first, "BINARY")) return fail(Status::Decode, "binary OFF files are not supported");
  std::int64_t dimension = 3;
  if (header.dimension) {
    if (!parse_int(first, dimension) || dimension != 3) {
      return fail(Status::Decode, "only three-dimensional OFF files are supported");
    }
    first = counts_in.next();
  }
  std::int64_t vertex_count = 0, face_count = 0, edge_count = 0;
  if (!parse_int(first, vertex_count) || !parse_int(counts_in.next(), face_count) ||
      !parse_int(counts_in.next(), edge_count) || vertex_count < 0 || face_count < 0) {
    return fail(Status::Decode, "malformed OFF element counts");
  }
  // Every vertex needs at least 6 bytes of text ("0 0 0\n"), every face 8.
  CVT_TRY(check_records(ctx, static_cast<std::uint64_t>(vertex_count), 6, text.size(), "vertices"));
  CVT_TRY(check_records(ctx, static_cast<std::uint64_t>(face_count), 8, text.size(), "faces"));

  ir::Mesh mesh;
  mesh.positions.reserve(static_cast<std::size_t>(vertex_count));
  std::vector<std::array<double, 4>> raw_colors;
  bool byte_colors = false;
  // Vertex lines: x y z [nx ny nz] [r g b [a]] [s t]. MeshLab writes the
  // colour before the normal, so with both present the order is decided
  // on the first line (a colour has no negative components).
  int colour_first = -1;  // -1 undecided, 0 standard, 1 colour before normal
  std::vector<double> values;
  for (std::int64_t v = 0; v < vertex_count; ++v) {
    if ((v & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(0.5 * static_cast<double>(v) / static_cast<double>(vertex_count)));
    values.clear();
    const std::string_view head = counts_in.next();
    double value = 0;
    if (!parse_double(head, value)) return fail(Status::Decode, "malformed OFF vertex");
    values.push_back(value);
    for (const std::string_view token : counts_in.rest_of_line()) {
      if (!parse_double(token, value)) return fail(Status::Decode, "malformed OFF vertex");
      values.push_back(value);
    }
    const std::size_t fixed = 3 + (header.normals ? 3 : 0) + (header.texcoords ? 2 : 0);
    if (values.size() < fixed) return fail(Status::Decode, "malformed OFF vertex on vertex {}", v);
    std::size_t colour_components = header.colors ? std::min<std::size_t>(values.size() - fixed, 4) : 0;
    if (header.colors && colour_components < 3) return fail(Status::Decode, "malformed OFF vertex colour");
    std::size_t normal_at = 3;
    std::size_t colour_at = header.normals ? 6 : 3;
    if (header.colors && header.normals) {
      if (colour_first < 0) {
        bool standard_ok = true;
        for (std::size_t k = 0; k < colour_components; ++k) standard_ok = standard_ok && values[6 + k] >= 0.0;
        colour_first = standard_ok ? 0 : 1;
      }
      if (colour_first == 1) {
        colour_at = 3;
        normal_at = 3 + colour_components;
      }
    }
    const auto f = [&](std::size_t i) { return static_cast<float>(values[i]); };
    mesh.positions.push_back({f(0), f(1), f(2)});
    if (header.normals) mesh.normals.push_back({f(normal_at), f(normal_at + 1), f(normal_at + 2)});
    if (header.colors) {
      std::array<double, 4> c{1, 1, 1, -1};
      for (std::size_t k = 0; k < colour_components; ++k) {
        c[k] = values[colour_at + k];
        if (c[k] > 1.0) byte_colors = true;
      }
      raw_colors.push_back(c);
    }
    if (header.texcoords) {
      const std::size_t at = 3 + (header.normals ? 3 : 0) + colour_components;
      mesh.texcoords.push_back({f(at), f(at + 1)});
    }
  }
  for (const auto& c : raw_colors) {
    // OFF colours are display (sRGB) values, either 0..255 or 0..1.
    mesh.colors.push_back(color_from_srgb(c[0], c[1], c[2], c[3], byte_colors ? 255.0 : 1.0));
  }

  bool all_triangles = true;
  std::size_t face_colors = 0;
  for (std::int64_t f = 0; f < face_count; ++f) {
    if ((f & 0xFFFF) == 0) {
      CVT_TRY(ctx.checkpoint(0.5 + 0.5 * static_cast<double>(f) / static_cast<double>(face_count)));
    }
    std::int64_t n = 0;
    if (!parse_int(counts_in.next(), n) || n < 1) return fail(Status::Decode, "malformed OFF face");
    if (n > 1'000'000) return fail(Status::Limit, "an OFF face with {} corners is too large", n);
    std::vector<std::uint32_t> corners;
    corners.reserve(static_cast<std::size_t>(n));
    for (std::int64_t k = 0; k < n; ++k) {
      std::int64_t index = 0;
      if (!parse_int(counts_in.next(), index)) return fail(Status::Decode, "malformed OFF face");
      if (index < 0 || index >= vertex_count) return fail(Status::Decode, "OFF face refers to missing vertex {}", index);
      corners.push_back(static_cast<std::uint32_t>(index));
    }
    if (!counts_in.rest_of_line().empty()) ++face_colors;
    CVT_TRY(check_elements(ctx, mesh.indices.size() + corners.size(), "face corners"));
    if (n < 3) continue;  // points and lines are not faces
    mesh.indices.insert(mesh.indices.end(), corners.begin(), corners.end());
    mesh.face_sizes.push_back(static_cast<std::uint32_t>(n));
    if (n != 3) all_triangles = false;
  }
  if (all_triangles) mesh.face_sizes.clear();
  if (face_colors > 0) ctx.warn("Per-face colours of the OFF file were not kept.");
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

constexpr WriterTraits off_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.materials = false;
  traits.point_clouds = true;
  return traits;
}

Result<Bytes> write_off(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, off_traits()));
  const int precision = write_precision(ctx);
  const bool uv = !mesh.texcoords.empty();
  const bool color = !mesh.colors.empty();
  const bool normal = !mesh.normals.empty();
  std::string out;
  out.reserve(mesh.positions.size() * 40 + mesh.indices.size() * 8 + 64);
  if (uv) out += "ST";
  if (color) out += "C";
  if (normal) out += "N";
  out += "OFF\n";
  out += std::format("{} {} 0\n", mesh.positions.size(), mesh.face_count());
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    if ((i & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(0.8 + 0.1 * static_cast<double>(i) / static_cast<double>(mesh.positions.size())));
    const ir::Vec3& p = mesh.positions[i];
    append_number(out, p.x, precision);
    out += ' ';
    append_number(out, p.y, precision);
    out += ' ';
    append_number(out, p.z, precision);
    if (normal) {
      for (const float c : {mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z}) {
        out += ' ';
        append_number(out, c, precision);
      }
    }
    if (color) {
      const auto c = color_to_srgb8(mesh.colors[i]);
      out += std::format(" {} {} {} {}", c[0], c[1], c[2], c[3]);
    }
    if (uv) {
      out += ' ';
      append_number(out, mesh.texcoords[i].x, precision);
      out += ' ';
      append_number(out, mesh.texcoords[i].y, precision);
    }
    out += '\n';
  }
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  for (std::size_t f = 0; f + 1 < offsets.size(); ++f) {
    out += std::to_string(offsets[f + 1] - offsets[f]);
    for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) {
      out += ' ';
      out += std::to_string(mesh.indices[c]);
    }
    out += '\n';
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

int sniff_off(ByteView data, std::string_view /*filename*/) {
  std::string_view head = text_head(data, 256);
  while (!head.empty() && head.front() == '#') {  // leading comment lines
    const std::size_t eol = head.find('\n');
    if (eol == std::string_view::npos) return 0;
    head.remove_prefix(eol + 1);
  }
  const std::string_view word = next_token(head);
  OffHeader header;
  if (!parse_off_keyword(word, header)) return 0;
  // The keyword must be followed by white space and a number (or BINARY).
  const std::string_view following = next_token(head);
  std::int64_t number = 0;
  if (parse_int(following, number) || following == "BINARY") return 92;
  return 40;
}

}  // namespace

void register_off(Registry& registry) {
  FormatInfo info;
  info.id = "off";
  info.name = "Object File Format model";
  info.description = "Simple text polygon mesh format from Geomview (OFF, COFF, NOFF).";
  info.category = Category::Model3D;
  info.extensions = {"off", "coff", "noff"};
  info.mime = "model/x-off";
  info.popularity = 25;
  info.sniff = sniff_off;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "off";
  codec.kind = Kind::Mesh;
  codec.read = read_off;
  codec.write = write_off;
  codec.write_options = mesh_write_options(off_traits());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::mesh
