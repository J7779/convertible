// Convertible core - mesh family: STL (binary and ASCII).
//
// Binary STL: an 80-byte header, a 32-bit triangle count, then 50 bytes per
// triangle (normal, three corners, a 16-bit attribute). Many binary files
// start their header with "solid", so the size equation decides first.
// Colours in the attribute follow VisCAM/SolidView (bit 15 set = valid,
// blue in bits 0-4, green 5-9, red 10-14) unless the header announces
// Materialise Magics ("COLOR=" default colour; red in bits 0-4, bit 15
// clear = the facet's own colour).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

enum class StlKind { Binary, Ascii, Unknown };

bool exact_binary_size(ByteView data) {
  if (data.size() < 84) return false;
  util::ByteReader r(data);
  r.skip(80);
  const std::uint64_t count = r.u32le();
  return 84 + count * 50 == data.size();
}

bool ascii_markers(std::string_view head) {
  std::string_view probe = util::trim_left(head);
  if (!util::istarts_with(probe, "solid")) return false;
  return head.find("facet") != std::string_view::npos || head.find("endsolid") != std::string_view::npos;
}

StlKind classify(ByteView data) {
  if (exact_binary_size(data)) return StlKind::Binary;
  const std::string_view head = text_head(data, 2048);
  if (ascii_markers(head) && looks_textual(data)) return StlKind::Ascii;
  if (data.size() >= 84) {
    util::ByteReader r(data);
    r.skip(80);
    const std::uint64_t count = r.u32le();
    if (count > 0 && 84 + count * 50 <= data.size()) return StlKind::Binary;  // trailing bytes
  }
  return StlKind::Unknown;
}

// Corners are emitted unwelded (three per facet, with the facet colour if
// any) and then welded exactly, so colour boundaries keep separate vertices.
Result<void> finish_stl(ir::Mesh& mesh, ReadContext& ctx) {
  weld(mesh, 0.0f, false, 0.0f);
  return finish_read(mesh, ctx);
}

Result<AnyIR> read_binary(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const std::string_view header = r.chars(80);
  const std::uint32_t count = r.u32le();
  if (!r.ok()) return fail(Status::Decode, "the STL file is truncated");
  CVT_TRY(check_records(ctx, count, 50, r.remaining(), "triangles"));
  if (84 + std::uint64_t{count} * 50 < ctx.input.size()) ctx.warn("Ignored bytes after the last STL triangle.");

  // Materialise: "COLOR=" followed by four bytes in the header.
  const std::size_t color_at = header.find("COLOR=");
  const bool magics = color_at != std::string_view::npos && color_at + 10 <= header.size();
  ir::Color default_color{1, 1, 1, 1};
  if (magics) {
    const auto byte = [&](std::size_t i) { return static_cast<double>(static_cast<unsigned char>(header[color_at + 6 + i])); };
    default_color = color_from_srgb(byte(0), byte(1), byte(2), byte(3));
  }

  ir::Mesh mesh;
  mesh.positions.reserve(std::size_t{count} * 3);
  std::vector<ir::Color> colors;
  bool any_color = false;
  for (std::uint32_t t = 0; t < count; ++t) {
    if ((t & 0x3FFF) == 0) CVT_TRY(ctx.checkpoint(0.8 * t / count));
    r.skip(12);  // facet normal: recomputed from the geometry when needed
    for (int k = 0; k < 3; ++k) {
      const float x = r.f32le();
      const float y = r.f32le();
      const float z = r.f32le();
      mesh.positions.push_back({x, y, z});
    }
    const std::uint16_t attribute = r.u16le();
    ir::Color c = default_color;
    if (magics) {
      if ((attribute & 0x8000) == 0) {
        c = color_from_srgb((attribute & 31) * 255.0 / 31.0, ((attribute >> 5) & 31) * 255.0 / 31.0,
                            ((attribute >> 10) & 31) * 255.0 / 31.0);
        any_color = true;
      }
    } else if ((attribute & 0x8000) != 0) {
      c = color_from_srgb(((attribute >> 10) & 31) * 255.0 / 31.0, ((attribute >> 5) & 31) * 255.0 / 31.0,
                          (attribute & 31) * 255.0 / 31.0);
      any_color = true;
    }
    colors.insert(colors.end(), 3, c);
  }
  if (!r.ok()) return fail(Status::Decode, "the STL file is truncated");
  mesh.indices.resize(mesh.positions.size());
  for (std::size_t i = 0; i < mesh.indices.size(); ++i) mesh.indices[i] = static_cast<std::uint32_t>(i);
  if (any_color || magics) mesh.colors = std::move(colors);
  std::string_view name = util::trim(header);
  if (util::istarts_with(name, "solid")) name = util::trim(name.substr(5));
  if (!name.empty() && !magics && util::is_valid_utf8(name) && name.find('\0') == std::string_view::npos) {
    mesh.metadata["comment"] = std::string(name);
  }
  CVT_TRY(finish_stl(mesh, ctx));
  return mesh;
}

Result<AnyIR> read_ascii(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  LineReader lines(text);
  std::string_view line;
  ir::Mesh mesh;
  std::vector<ir::Vec3> loop;
  bool in_solid = false;
  std::size_t solids = 0;
  while (lines.next(line)) {
    if ((lines.line_number() & 0x3FFF) == 0) {
      CVT_TRY(ctx.checkpoint(0.8 * static_cast<double>(lines.position()) / (text.size() + 1)));
    }
    std::string_view rest = line;
    const std::string_view keyword = next_token(rest);
    if (keyword.empty()) continue;
    if (util::iequals(keyword, "solid")) {
      in_solid = true;
      ++solids;
      const std::string name(util::trim(rest));
      mesh.objects.push_back({name, mesh.face_count(), 0, {}});
      if (solids == 1) mesh.name = name;
    } else if (util::iequals(keyword, "vertex")) {
      ir::Vec3 p;
      if (!parse_float(next_token(rest), p.x) || !parse_float(next_token(rest), p.y) ||
          !parse_float(next_token(rest), p.z)) {
        return fail(Status::Decode, "malformed STL vertex on line {}", lines.line_number());
      }
      loop.push_back(p);
      if (loop.size() > 4096) return fail(Status::Decode, "an STL facet has too many vertices");
    } else if (util::iequals(keyword, "endloop")) {
      if (loop.size() >= 3) {
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (std::size_t k = 1; k + 1 < loop.size(); ++k) {  // some writers emit polygons
          mesh.indices.insert(mesh.indices.end(), {base, base + static_cast<std::uint32_t>(k),
                                                   base + static_cast<std::uint32_t>(k + 1)});
          if (!mesh.objects.empty()) mesh.objects.back().face_count++;
        }
        mesh.positions.insert(mesh.positions.end(), loop.begin(), loop.end());
        CVT_TRY(check_elements(ctx, mesh.positions.size(), "vertices"));
      }
      loop.clear();
    } else if (util::iequals(keyword, "endsolid")) {
      in_solid = false;
    } else if (!util::iequals(keyword, "facet") && !util::iequals(keyword, "outer") &&
               !util::iequals(keyword, "endfacet") && !util::iequals(keyword, "color")) {
      return fail(Status::Decode, "unexpected '{}' on line {} of an ASCII STL file", keyword, lines.line_number());
    }
  }
  if (in_solid && mesh.positions.empty()) return fail(Status::Decode, "the ASCII STL file is truncated");
  if (mesh.objects.size() <= 1) mesh.objects.clear();
  CVT_TRY(finish_stl(mesh, ctx));
  return mesh;
}

Result<AnyIR> read_stl(ReadContext& ctx) {
  switch (classify(ctx.input)) {
    case StlKind::Binary: return read_binary(ctx);
    case StlKind::Ascii: return read_ascii(ctx);
    case StlKind::Unknown: break;
  }
  return fail(Status::Decode, "not an STL file");
}

constexpr WriterTraits stl_traits() {
  WriterTraits traits;
  traits.triangles_only = true;
  traits.normals = false;
  traits.texcoords = false;
  traits.materials = false;
  traits.text = true;
  return traits;
}

ir::Vec3 facet_normal(const ir::Mesh& mesh, std::size_t t) {
  const ir::Vec3& a = mesh.positions[mesh.indices[t]];
  const ir::Vec3& b = mesh.positions[mesh.indices[t + 1]];
  const ir::Vec3& c = mesh.positions[mesh.indices[t + 2]];
  return normalize(cross(sub(b, a), sub(c, a)));
}

Result<Bytes> write_stl(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, stl_traits()));
  const std::size_t triangles = mesh.indices.size() / 3;
  const bool binary = ctx.options.get_bool("binary", true);
  if (binary) {
    if (triangles > 0xFFFFFFFFull) return fail(Status::Encode, "too many triangles for STL");
    const bool colors = !mesh.colors.empty();
    util::ByteWriter out(84 + triangles * 50);
    std::string header = "Binary STL";
    if (!options::resolved_strip_metadata(ctx.options) && !mesh.name.empty()) {
      header += " - ";
      for (const char c : mesh.name) header.push_back(static_cast<unsigned char>(c) >= 32 ? c : ' ');
    }
    header.resize(80, ' ');
    out.text(header);
    out.u32le(static_cast<std::uint32_t>(triangles));
    for (std::size_t t = 0; t < triangles; ++t) {
      if ((t & 0x3FFF) == 0) CVT_TRY(ctx.checkpoint(0.8 + 0.2 * static_cast<double>(t) / triangles));
      const ir::Vec3 n = facet_normal(mesh, t * 3);
      out.f32le(n.x).f32le(n.y).f32le(n.z);
      for (int k = 0; k < 3; ++k) {
        const ir::Vec3& p = mesh.positions[mesh.indices[t * 3 + k]];
        out.f32le(p.x).f32le(p.y).f32le(p.z);
      }
      std::uint16_t attribute = 0;
      if (colors) {  // VisCAM/SolidView 5:5:5 with the "valid" bit; average of the corners
        double rgb[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
          const ir::Color& c = mesh.colors[mesh.indices[t * 3 + k]];
          rgb[0] += c.r / 3.0;
          rgb[1] += c.g / 3.0;
          rgb[2] += c.b / 3.0;
        }
        const auto five = [](double linear) {
          return static_cast<std::uint16_t>(std::lround(linear_to_srgb(static_cast<float>(linear)) * 31.0f));
        };
        attribute = static_cast<std::uint16_t>(0x8000 | (five(rgb[0]) << 10) | (five(rgb[1]) << 5) | five(rgb[2]));
      }
      out.u16le(attribute);
    }
    CVT_TRY(ctx.limits.check_output(out.size()));
    return out.take();
  }
  const int precision = write_precision(ctx);
  std::string name;
  for (const char c : mesh.name) name.push_back(util::is_ascii_space(c) ? '_' : c);
  if (options::resolved_strip_metadata(ctx.options) || name.empty()) name = "model";
  std::string out;
  out.reserve(triangles * 260 + 64);
  out += std::format("solid {}\n", name);
  const auto vec = [&](const ir::Vec3& v) {
    append_number(out, v.x, precision);
    out += ' ';
    append_number(out, v.y, precision);
    out += ' ';
    append_number(out, v.z, precision);
    out += '\n';
  };
  for (std::size_t t = 0; t < triangles; ++t) {
    if ((t & 0x3FFF) == 0) CVT_TRY(ctx.checkpoint(0.8 + 0.2 * static_cast<double>(t) / triangles));
    out += "  facet normal ";
    vec(facet_normal(mesh, t * 3));
    out += "    outer loop\n";
    for (int k = 0; k < 3; ++k) {
      out += "      vertex ";
      vec(mesh.positions[mesh.indices[t * 3 + k]]);
    }
    out += "    endloop\n  endfacet\n";
  }
  out += std::format("endsolid {}\n", name);
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

int sniff_stl(ByteView data, std::string_view filename) {
  if (exact_binary_size(data)) {
    // 84 + 50 n is a weak signal on its own; the extension or an empty-ish
    // header makes it convincing.
    return has_extension(filename, "stl") ? 95 : 72;
  }
  const std::string_view head = text_head(data, 2048);
  if (ascii_markers(head) && looks_textual(data)) {
    return head.find("facet normal") != std::string_view::npos || head.find("outer loop") != std::string_view::npos
               ? 93
               : 60;
  }
  return 0;
}

}  // namespace

void register_stl(Registry& registry) {
  FormatInfo info;
  info.id = "stl";
  info.name = "STL model";
  info.description = "Triangle mesh format used by 3D printers and CAD (binary or ASCII).";
  info.category = Category::Model3D;
  info.extensions = {"stl"};
  info.mime = "model/stl";
  info.popularity = 75;
  info.sniff = sniff_stl;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "stl";
  codec.kind = Kind::Mesh;
  codec.read = read_stl;
  codec.write = write_stl;
  codec.write_options = mesh_write_options(stl_traits());
  codec.write_options.insert(codec.write_options.begin(),
                             OptionSpec::boolean("binary", "Binary", true,
                                                 "Write compact binary STL; off writes the larger ASCII form."));
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::mesh
