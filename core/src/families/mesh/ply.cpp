// Convertible core - mesh family: Stanford PLY (ASCII, binary little and big endian).
//
// A header declares elements ("vertex 8") with scalar and list properties;
// the body holds the records in order. Every element and property type is
// read (unknown ones are skipped correctly); vertex x/y/z, normals,
// colours, texture coordinates and other scalar vertex properties map to
// the IR; face vertex lists become polygons, per-face colours and
// per-corner texture coordinates split vertices.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
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

enum class PlyType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64, Invalid };

PlyType parse_type(std::string_view name) {
  if (name == "char" || name == "int8") return PlyType::Int8;
  if (name == "uchar" || name == "uint8") return PlyType::UInt8;
  if (name == "short" || name == "int16") return PlyType::Int16;
  if (name == "ushort" || name == "uint16") return PlyType::UInt16;
  if (name == "int" || name == "int32") return PlyType::Int32;
  if (name == "uint" || name == "uint32") return PlyType::UInt32;
  if (name == "float" || name == "float32") return PlyType::Float32;
  if (name == "double" || name == "float64") return PlyType::Float64;
  return PlyType::Invalid;
}

std::size_t type_size(PlyType type) {
  switch (type) {
    case PlyType::Int8:
    case PlyType::UInt8: return 1;
    case PlyType::Int16:
    case PlyType::UInt16: return 2;
    case PlyType::Int32:
    case PlyType::UInt32:
    case PlyType::Float32: return 4;
    case PlyType::Float64: return 8;
    case PlyType::Invalid: break;
  }
  return 0;
}

struct PlyProperty {
  std::string name;
  PlyType type = PlyType::Invalid;
  bool list = false;
  PlyType count_type = PlyType::Invalid;
};

struct PlyElement {
  std::string name;
  std::uint64_t count = 0;
  std::vector<PlyProperty> properties;
};

enum class PlyFormat { Ascii, BinaryLE, BinaryBE };

struct PlyHeader {
  PlyFormat format = PlyFormat::Ascii;
  std::vector<PlyElement> elements;
  std::vector<std::string> comments;
  std::vector<std::string> texture_files;
  std::size_t body_offset = 0;
};

Result<PlyHeader> parse_header(ByteView input) {
  const std::string_view text = as_chars(input);
  if (!text.starts_with("ply")) return fail(Status::Decode, "not a PLY file");
  const std::size_t end = text.find("end_header");
  if (end == std::string_view::npos || end > (1u << 20)) return fail(Status::Decode, "the PLY header is incomplete");
  std::size_t body = end + 10;
  if (body < text.size() && text[body] == '\r') ++body;
  if (body < text.size() && text[body] == '\n') ++body;
  PlyHeader header;
  header.body_offset = body;
  LineReader lines(text.substr(0, end));
  std::string_view line;
  bool have_format = false;
  while (lines.next(line)) {
    std::string_view rest = line;
    const std::string_view keyword = next_token(rest);
    if (keyword.empty() || keyword == "ply") continue;
    if (keyword == "format") {
      const std::string_view kind = next_token(rest);
      if (kind == "ascii") header.format = PlyFormat::Ascii;
      else if (kind == "binary_little_endian") header.format = PlyFormat::BinaryLE;
      else if (kind == "binary_big_endian") header.format = PlyFormat::BinaryBE;
      else return fail(Status::Decode, "unknown PLY format '{}'", kind);
      have_format = true;
    } else if (keyword == "comment" || keyword == "obj_info") {
      std::string_view value = util::trim(rest);
      std::string_view probe = value;
      if (keyword == "comment" && util::iequals(next_token(probe), "TextureFile")) {
        header.texture_files.emplace_back(util::trim(probe));
      } else if (!value.empty()) {
        header.comments.emplace_back(value);
      }
    } else if (keyword == "element") {
      PlyElement element;
      element.name = std::string(next_token(rest));
      std::int64_t count = 0;
      if (!parse_int(next_token(rest), count) || count < 0) return fail(Status::Decode, "malformed PLY element count");
      element.count = static_cast<std::uint64_t>(count);
      header.elements.push_back(std::move(element));
    } else if (keyword == "property") {
      if (header.elements.empty()) return fail(Status::Decode, "PLY property outside an element");
      PlyProperty property;
      std::string_view type = next_token(rest);
      if (type == "list") {
        property.list = true;
        property.count_type = parse_type(next_token(rest));
        type = next_token(rest);
        if (property.count_type == PlyType::Invalid || property.count_type == PlyType::Float32 ||
            property.count_type == PlyType::Float64) {
          return fail(Status::Decode, "unsupported PLY list count type");
        }
      }
      property.type = parse_type(type);
      if (property.type == PlyType::Invalid) return fail(Status::Decode, "unknown PLY property type '{}'", type);
      property.name = std::string(next_token(rest));
      header.elements.back().properties.push_back(std::move(property));
    }
  }
  if (!have_format) return fail(Status::Decode, "the PLY header has no format line");
  return header;
}

// Reads values of either encoding with one interface.
class PlyBody {
 public:
  PlyBody(ByteView data, PlyFormat format) : format_(format), reader_(data), text_(as_chars(data)) {}

  bool value(PlyType type, double& out) {
    if (format_ == PlyFormat::Ascii) {
      const std::string_view token = next_ascii();
      if (token.empty()) return false;
      if (!parse_double(token, out)) return false;
      return true;
    }
    const std::endian order = format_ == PlyFormat::BinaryLE ? std::endian::little : std::endian::big;
    switch (type) {
      case PlyType::Int8: out = reader_.read<std::int8_t>(order); break;
      case PlyType::UInt8: out = reader_.read<std::uint8_t>(order); break;
      case PlyType::Int16: out = reader_.read<std::int16_t>(order); break;
      case PlyType::UInt16: out = reader_.read<std::uint16_t>(order); break;
      case PlyType::Int32: out = reader_.read<std::int32_t>(order); break;
      case PlyType::UInt32: out = reader_.read<std::uint32_t>(order); break;
      case PlyType::Float32: out = std::bit_cast<float>(reader_.read<std::uint32_t>(order)); break;
      case PlyType::Float64: out = std::bit_cast<double>(reader_.read<std::uint64_t>(order)); break;
      case PlyType::Invalid: return false;
    }
    return reader_.ok();
  }
  [[nodiscard]] std::size_t remaining() const {
    return format_ == PlyFormat::Ascii ? text_.size() - pos_ : reader_.remaining();
  }
  [[nodiscard]] std::size_t consumed() const { return format_ == PlyFormat::Ascii ? pos_ : reader_.position(); }

 private:
  std::string_view next_ascii() {
    while (pos_ < text_.size() && util::is_ascii_space(text_[pos_])) ++pos_;
    const std::size_t start = pos_;
    while (pos_ < text_.size() && !util::is_ascii_space(text_[pos_])) ++pos_;
    return text_.substr(start, pos_ - start);
  }
  PlyFormat format_;
  util::ByteReader reader_;
  std::string_view text_;
  std::size_t pos_ = 0;
};

// Index of a property by any of the given names, or -1.
int find_property(const PlyElement& element, std::initializer_list<std::string_view> names) {
  for (std::size_t i = 0; i < element.properties.size(); ++i) {
    for (const std::string_view name : names) {
      if (element.properties[i].name == name) return static_cast<int>(i);
    }
  }
  return -1;
}

double color_scale(PlyType type) {
  switch (type) {
    case PlyType::UInt8:
    case PlyType::Int8: return 255.0;
    case PlyType::UInt16:
    case PlyType::Int16: return 65535.0;
    case PlyType::Float32:
    case PlyType::Float64: return 1.0;
    default: return 255.0;
  }
}

Result<AnyIR> read_ply(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const PlyHeader header, parse_header(ctx.input));
  PlyBody body(ctx.input.subspan(std::min(header.body_offset, ctx.input.size())), header.format);
  ir::Mesh mesh;
  for (const std::string& comment : header.comments) {
    auto& slot = mesh.metadata["comment"];
    if (!slot.empty()) slot += '\n';
    slot += comment;
  }
  for (const std::string& file : header.texture_files) {
    ir::Texture texture;
    texture.uri = file;
    texture.name = std::string(base_name(file));
    mesh.textures.push_back(std::move(texture));
    ctx.warn(std::format("Texture '{}' is a separate file and was not included.", file));
  }
  std::vector<ir::Vec2> corner_uvs;  // per face corner (face "texcoord" lists)
  std::vector<ir::Color> face_colors;
  bool have_vertex = false;
  std::uint64_t total_records = 0;
  for (const PlyElement& e : header.elements) total_records += e.count;
  std::uint64_t done = 0;
  std::vector<double> scalars;
  std::vector<double> list;
  for (const PlyElement& element : header.elements) {
    // Every record takes at least one byte (binary) or two characters.
    std::size_t min_record = 0;
    for (const PlyProperty& p : element.properties) min_record += p.list ? type_size(p.count_type) : type_size(p.type);
    if (header.format == PlyFormat::Ascii) min_record = element.properties.empty() ? 0 : 2 * element.properties.size();
    CVT_TRY(check_records(ctx, element.count, std::max<std::size_t>(min_record, element.properties.empty() ? 0 : 1),
                          body.remaining(), element.name));
    const bool is_vertex = element.name == "vertex" && !have_vertex;
    const bool is_face = element.name == "face";
    int ix = -1, iy = -1, iz = -1, inx = -1, iny = -1, inz = -1, ir_ = -1, ig = -1, ib = -1, ia = -1, iu = -1, iv = -1;
    int list_index = -1, list_uv = -1;
    std::vector<int> extra;  // vertex scalar properties kept as attributes
    if (is_vertex) {
      ix = find_property(element, {"x"});
      iy = find_property(element, {"y"});
      iz = find_property(element, {"z"});
      if (ix < 0 || iy < 0 || iz < 0) return fail(Status::Decode, "the PLY vertices have no x, y, z");
      inx = find_property(element, {"nx", "normal_x"});
      iny = find_property(element, {"ny", "normal_y"});
      inz = find_property(element, {"nz", "normal_z"});
      ir_ = find_property(element, {"red", "r", "diffuse_red"});
      ig = find_property(element, {"green", "g", "diffuse_green"});
      ib = find_property(element, {"blue", "b", "diffuse_blue"});
      ia = find_property(element, {"alpha", "a", "diffuse_alpha"});
      iu = find_property(element, {"s", "u", "texture_u", "texture_s"});
      iv = find_property(element, {"t", "v", "texture_v", "texture_t"});
      for (int i = 0; i < static_cast<int>(element.properties.size()); ++i) {
        if (element.properties[i].list) continue;
        if (i == ix || i == iy || i == iz || i == inx || i == iny || i == inz || i == ir_ || i == ig || i == ib ||
            i == ia || i == iu || i == iv) {
          continue;
        }
        extra.push_back(i);
        mesh.attributes.push_back({element.properties[i].name, {}});
      }
      mesh.positions.reserve(static_cast<std::size_t>(element.count));
    }
    if (is_face) {
      list_index = find_property(element, {"vertex_indices", "vertex_index"});
      list_uv = find_property(element, {"texcoord"});
      ir_ = find_property(element, {"red", "diffuse_red"});
      ig = find_property(element, {"green", "diffuse_green"});
      ib = find_property(element, {"blue", "diffuse_blue"});
      ia = find_property(element, {"alpha", "diffuse_alpha"});
      if (list_index < 0 || !element.properties[static_cast<std::size_t>(list_index)].list) {
        return fail(Status::Decode, "the PLY faces have no vertex index list");
      }
    }
    scalars.assign(element.properties.size(), 0.0);
    for (std::uint64_t r = 0; r < element.count; ++r) {
      if ((done++ & 0x3FFF) == 0) CVT_TRY(ctx.checkpoint(0.9 * static_cast<double>(done) / static_cast<double>(total_records + 1)));
      for (std::size_t p = 0; p < element.properties.size(); ++p) {
        const PlyProperty& property = element.properties[p];
        if (!property.list) {
          if (!body.value(property.type, scalars[p])) return fail(Status::Decode, "the PLY data is truncated or malformed");
          continue;
        }
        double count_value = 0;
        if (!body.value(property.count_type, count_value) || count_value < 0 || count_value > 1e6) {
          return fail(Status::Decode, "malformed PLY list");
        }
        const auto count = static_cast<std::size_t>(count_value);
        const bool keep = is_face && (static_cast<int>(p) == list_index || static_cast<int>(p) == list_uv);
        if (keep) list.clear();
        for (std::size_t k = 0; k < count; ++k) {
          double v = 0;
          if (!body.value(property.type, v)) return fail(Status::Decode, "the PLY data is truncated or malformed");
          if (keep) list.push_back(v);
        }
        if (!keep) continue;
        if (static_cast<int>(p) == list_index) {
          if (count < 3) continue;  // points and edges are not faces
          for (const double index : list) {
            if (!(index >= 0) || index >= 4294967295.0) return fail(Status::Decode, "negative PLY vertex index");
            mesh.indices.push_back(static_cast<std::uint32_t>(index));
          }
          mesh.face_sizes.push_back(static_cast<std::uint32_t>(count));
          CVT_TRY(check_elements(ctx, mesh.indices.size(), "face corners"));
        } else {
          // Per-corner uv: pairs; must match the corner count of this face.
          for (std::size_t k = 0; k + 1 < list.size(); k += 2) {
            corner_uvs.push_back({static_cast<float>(list[k]), static_cast<float>(list[k + 1])});
          }
        }
      }
      if (is_vertex) {
        const auto f = [&](int i) { return static_cast<float>(scalars[static_cast<std::size_t>(i)]); };
        mesh.positions.push_back({f(ix), f(iy), f(iz)});
        if (inx >= 0 && iny >= 0 && inz >= 0) mesh.normals.push_back({f(inx), f(iny), f(inz)});
        if (ir_ >= 0 && ig >= 0 && ib >= 0) {
          const double scale = color_scale(element.properties[static_cast<std::size_t>(ir_)].type);
          mesh.colors.push_back(color_from_srgb(scalars[ir_], scalars[ig], scalars[ib], ia >= 0 ? scalars[ia] : -1.0,
                                                scale));
        }
        if (iu >= 0 && iv >= 0) mesh.texcoords.push_back({f(iu), f(iv)});
        for (std::size_t k = 0; k < extra.size(); ++k) mesh.attributes[k].values.push_back(f(extra[k]));
      } else if (is_face && ir_ >= 0 && ig >= 0 && ib >= 0) {
        const double scale = color_scale(element.properties[static_cast<std::size_t>(ir_)].type);
        face_colors.push_back(color_from_srgb(scalars[ir_], scalars[ig], scalars[ib], ia >= 0 ? scalars[ia] : -1.0, scale));
      }
    }
    if (is_vertex) have_vertex = true;
  }
  if (!have_vertex) return fail(Status::Decode, "the PLY file has no vertex element");
  for (const std::uint32_t index : mesh.indices) {
    if (index >= mesh.positions.size()) return fail(Status::Decode, "a PLY face refers to missing vertex {}", index);
  }
  // Per-face colours or per-corner uvs: give every corner its own vertex,
  // then merge identical ones again.
  const bool split_colors = !face_colors.empty() && face_colors.size() == mesh.face_sizes.size() && mesh.colors.empty();
  const bool split_uvs = !corner_uvs.empty() && corner_uvs.size() == mesh.indices.size() && mesh.texcoords.empty();
  if (!face_colors.empty() && !split_colors) ctx.warn("Per-face colours of the PLY file were not kept.");
  if (split_colors || split_uvs) {
    ir::Mesh split = mesh;
    split.positions.clear();
    split.normals.clear();
    split.colors.clear();
    split.texcoords.clear();
    for (ir::VertexAttribute& a : split.attributes) a.values.clear();
    std::size_t corner = 0;
    for (std::size_t f = 0; f < mesh.face_sizes.size(); ++f) {
      for (std::uint32_t k = 0; k < mesh.face_sizes[f]; ++k, ++corner) {
        const std::uint32_t old = mesh.indices[corner];
        split.indices[corner] = static_cast<std::uint32_t>(split.positions.size());
        split.positions.push_back(mesh.positions[old]);
        if (!mesh.normals.empty()) split.normals.push_back(mesh.normals[old]);
        if (split_colors) split.colors.push_back(face_colors[f]);
        else if (!mesh.colors.empty()) split.colors.push_back(mesh.colors[old]);
        if (split_uvs) split.texcoords.push_back(corner_uvs[corner]);
        else if (!mesh.texcoords.empty()) split.texcoords.push_back(mesh.texcoords[old]);
        for (std::size_t a = 0; a < mesh.attributes.size(); ++a) split.attributes[a].values.push_back(mesh.attributes[a].values[old]);
      }
    }
    mesh = std::move(split);
    weld(mesh, 0.0f, false, 0.0f);
  }
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- writing

constexpr WriterTraits ply_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.point_clouds = true;
  traits.materials = false;
  return traits;
}

Result<Bytes> write_ply(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, ply_traits()));
  const bool binary = ctx.options.get_bool("binary", true);
  const bool big = ctx.options.get_bool("big_endian", false);
  const std::endian order = big ? std::endian::big : std::endian::little;
  const int precision = write_precision(ctx);
  const bool normals = !mesh.normals.empty();
  const bool colors = !mesh.colors.empty();
  const bool uvs = !mesh.texcoords.empty();
  const std::size_t faces = mesh.point_cloud ? 0 : mesh.face_count();
  std::uint32_t largest = 3;
  for (const std::uint32_t s : mesh.face_sizes) largest = std::max(largest, s);
  const bool wide_counts = largest > 255;

  std::string header = "ply\n";
  header += binary ? (big ? "format binary_big_endian 1.0\n" : "format binary_little_endian 1.0\n") : "format ascii 1.0\n";
  if (!options::resolved_strip_metadata(ctx.options)) {
    for (const auto& [key, text] : mesh.metadata) {
      for (const std::string_view line : util::split_lines(text)) header += std::format("comment {}\n", line);
    }
  }
  for (const ir::Texture& texture : mesh.textures) {
    if (!texture.uri.empty()) header += std::format("comment TextureFile {}\n", texture.uri);
  }
  header += std::format("element vertex {}\nproperty float x\nproperty float y\nproperty float z\n", mesh.positions.size());
  if (normals) header += "property float nx\nproperty float ny\nproperty float nz\n";
  if (uvs) header += "property float s\nproperty float t\n";
  if (colors) header += "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty uchar alpha\n";
  for (const ir::VertexAttribute& a : mesh.attributes) {
    std::string name;
    for (const char c : a.name) name.push_back(util::is_ascii_space(c) ? '_' : c);
    header += std::format("property float {}\n", name.empty() ? "value" : name);
  }
  if (faces > 0) {
    header += std::format("element face {}\nproperty list {} int vertex_indices\n", faces, wide_counts ? "uint" : "uchar");
  }
  header += "end_header\n";

  util::ByteWriter out(header.size() + mesh.positions.size() * 40 + mesh.indices.size() * 4 + faces);
  out.text(header);
  std::string line;
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    if ((i & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(0.8 + 0.1 * static_cast<double>(i) / static_cast<double>(mesh.positions.size())));
    std::array<std::uint8_t, 4> rgba{};
    if (colors) rgba = color_to_srgb8(mesh.colors[i]);
    if (binary) {
      const auto f32 = [&](float v) { out.write(std::bit_cast<std::uint32_t>(v), order); };
      f32(mesh.positions[i].x);
      f32(mesh.positions[i].y);
      f32(mesh.positions[i].z);
      if (normals) {
        f32(mesh.normals[i].x);
        f32(mesh.normals[i].y);
        f32(mesh.normals[i].z);
      }
      if (uvs) {
        f32(mesh.texcoords[i].x);
        f32(mesh.texcoords[i].y);
      }
      if (colors) out.u8(rgba[0]).u8(rgba[1]).u8(rgba[2]).u8(rgba[3]);
      for (const ir::VertexAttribute& a : mesh.attributes) f32(a.values[i]);
      continue;
    }
    line.clear();
    const auto num = [&](float v) {
      if (!line.empty()) line += ' ';
      append_number(line, v, precision);
    };
    num(mesh.positions[i].x);
    num(mesh.positions[i].y);
    num(mesh.positions[i].z);
    if (normals) {
      num(mesh.normals[i].x);
      num(mesh.normals[i].y);
      num(mesh.normals[i].z);
    }
    if (uvs) {
      num(mesh.texcoords[i].x);
      num(mesh.texcoords[i].y);
    }
    if (colors) line += std::format(" {} {} {} {}", rgba[0], rgba[1], rgba[2], rgba[3]);
    for (const ir::VertexAttribute& a : mesh.attributes) num(a.values[i]);
    line += '\n';
    out.text(line);
  }
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  for (std::size_t f = 0; f < faces; ++f) {
    const auto n = static_cast<std::uint32_t>(offsets[f + 1] - offsets[f]);
    if (binary) {
      if (wide_counts) out.write(n, order);
      else out.u8(static_cast<std::uint8_t>(n));
      for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) out.write(mesh.indices[c], order);
      continue;
    }
    line = std::to_string(n);
    for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) {
      line += ' ';
      line += std::to_string(mesh.indices[c]);
    }
    line += '\n';
    out.text(line);
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out.take();
}

}  // namespace

void register_ply(Registry& registry) {
  FormatInfo info;
  info.id = "ply";
  info.name = "Polygon File Format model";
  info.description = "Stanford PLY meshes and point clouds with colours and custom properties.";
  info.category = Category::Model3D;
  info.extensions = {"ply"};
  info.mime = "model/ply";
  info.popularity = 45;
  info.signatures = {Signature::at(0, "'ply' 0A"), Signature::at(0, "'ply' 0D 0A")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "ply";
  codec.kind = Kind::Mesh;
  codec.read = read_ply;
  codec.write = write_ply;
  codec.write_options = mesh_write_options(ply_traits());
  codec.write_options.insert(codec.write_options.begin(),
                             OptionSpec::boolean("binary", "Binary", true, "Write binary PLY; off writes ASCII text."));
  codec.write_options.push_back(
      OptionSpec::boolean("big_endian", "Big-endian", false, "Write binary data in big-endian byte order.").advanced());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::mesh
