// Convertible core - mesh family: VTK legacy files and Gmsh meshes.
//
// VTK legacy (".vtk"): POLYDATA and UNSTRUCTURED_GRID datasets in ASCII
// or big-endian BINARY encoding, with the classic cell lists and the
// OFFSETS/CONNECTIVITY layout of version 5; point data (normals, texture
// coordinates, colour scalars, scalars, field arrays) is read, cell data is
// skipped. Written as POLYDATA.
//
// Gmsh (".msh"): format 2.2 (ASCII and binary) and 4.1 (ASCII); surface
// elements (triangles, quadrilaterals and their higher-order variants by
// their corner nodes) become faces, physical names become groups.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

// Byte-level cursor that reads text lines/tokens and raw binary blocks.
class Cursor {
 public:
  explicit Cursor(ByteView data) : data_(data), text_(as_chars(data)) {}

  [[nodiscard]] bool at_end() const { return pos_ >= text_.size(); }
  [[nodiscard]] std::size_t position() const { return pos_; }
  [[nodiscard]] std::size_t remaining() const { return text_.size() - std::min(pos_, text_.size()); }

  void skip_space() {
    while (pos_ < text_.size() && util::is_ascii_space(text_[pos_])) ++pos_;
  }
  // The rest of the current line (after leading white space), consumed with its newline.
  std::string_view line() {
    skip_space();
    const std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
    std::string_view out = text_.substr(start, pos_ - start);
    if (pos_ < text_.size()) ++pos_;
    if (!out.empty() && out.back() == '\r') out.remove_suffix(1);
    return out;
  }
  // The line exactly as it is (no leading skip), consumed with its newline.
  std::string_view raw_line() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
    std::string_view out = text_.substr(start, pos_ - start);
    if (pos_ < text_.size()) ++pos_;
    if (!out.empty() && out.back() == '\r') out.remove_suffix(1);
    return out;
  }
  std::string_view token() {
    skip_space();
    const std::size_t start = pos_;
    while (pos_ < text_.size() && !util::is_ascii_space(text_[pos_])) ++pos_;
    return text_.substr(start, pos_ - start);
  }
  std::string_view peek_token() {
    const std::size_t save = pos_;
    const std::string_view t = token();
    pos_ = save;
    return t;
  }
  ByteView bytes(std::size_t count) {
    if (count > remaining()) {
      pos_ = text_.size();
      return {};
    }
    const ByteView out = data_.subspan(pos_, count);
    pos_ += count;
    return out;
  }
  void seek(std::size_t pos) { pos_ = std::min(pos, text_.size()); }

 private:
  ByteView data_;
  std::string_view text_;
  std::size_t pos_ = 0;
};

// --------------------------------------------------------------------------- VTK

std::size_t vtk_type_size(std::string_view type) {
  if (type == "unsigned_char" || type == "char" || type == "bit") return 1;
  if (type == "unsigned_short" || type == "short") return 2;
  if (type == "unsigned_int" || type == "int" || type == "float" || type == "vtkIdType") return 4;
  if (type == "unsigned_long" || type == "long" || type == "double" || type == "vtktypeint64" || type == "vtktypeuint64") return 8;
  return 0;
}

class VtkReader {
 public:
  enum class Data { None, Point, Cell };

  VtkReader(ReadContext& ctx) : ctx_(ctx), in_(ctx.input) {}

  Result<std::vector<double>> values(std::uint64_t count, std::string_view type) {
    CVT_TRY(check_elements(ctx_, count, "VTK values"));
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(count));
    if (!binary_) {
      for (std::uint64_t i = 0; i < count; ++i) {
        double v = 0;
        if (!parse_double(in_.token(), v)) return fail(Status::Decode, "the VTK data is truncated or malformed");
        out.push_back(v);
      }
      return out;
    }
    const std::size_t size = vtk_type_size(type);
    if (size == 0) return fail(Status::Decode, "unsupported VTK data type '{}'", type);
    if (count > in_.remaining() / size) return fail(Status::Decode, "the VTK data is truncated");
    const ByteView raw = in_.bytes(static_cast<std::size_t>(count) * size);
    util::ByteReader r(raw);
    for (std::uint64_t i = 0; i < count; ++i) {
      double v = 0;
      if (type == "float") v = r.f32be();
      else if (type == "double") v = r.f64be();
      else if (type == "unsigned_char" || type == "bit") v = r.u8();
      else if (type == "char") v = r.i8();
      else if (type == "unsigned_short") v = r.u16be();
      else if (type == "short") v = r.i16be();
      else if (type == "unsigned_int") v = r.u32be();
      else if (size == 4) v = r.i32be();
      else if (type == "unsigned_long" || type == "vtktypeuint64") v = static_cast<double>(r.u64be());
      else v = static_cast<double>(r.i64be());
      out.push_back(v);
    }
    return out;
  }

  // A cell list: classic "n size" + data, or version 5 OFFSETS/CONNECTIVITY.
  Result<std::vector<std::vector<std::int64_t>>> cells(std::uint64_t first, std::uint64_t second) {
    std::vector<std::vector<std::int64_t>> out;
    if (in_.peek_token() == "OFFSETS") {
      in_.token();
      const std::string type(in_.line());
      CVT_TRY_ASSIGN(const std::vector<double> offsets, values(first, util::trim(type)));
      if (in_.token() != "CONNECTIVITY") return fail(Status::Decode, "VTK CONNECTIVITY expected");
      const std::string type2(in_.line());
      CVT_TRY_ASSIGN(const std::vector<double> connectivity, values(second, util::trim(type2)));
      for (std::size_t c = 0; c + 1 < offsets.size(); ++c) {
        const std::uint64_t begin = file_index(offsets[c]);
        const std::uint64_t end = file_index(offsets[c + 1]);
        if (begin > end || end > connectivity.size()) return fail(Status::Decode, "malformed VTK cell offsets");
        std::vector<std::int64_t> cell;
        for (std::uint64_t k = begin; k < end; ++k) {
          const std::uint64_t id = file_index(connectivity[static_cast<std::size_t>(k)]);
          cell.push_back(id == invalid_index ? -1 : static_cast<std::int64_t>(id));
        }
        out.push_back(std::move(cell));
      }
      return out;
    }
    CVT_TRY_ASSIGN(const std::vector<double> data, values(second, "int"));
    std::size_t at = 0;
    for (std::uint64_t c = 0; c < first; ++c) {
      if (at >= data.size()) return fail(Status::Decode, "malformed VTK cell list");
      const std::uint64_t n = file_index(data[at++]);
      if (n > data.size() - at) return fail(Status::Decode, "malformed VTK cell list");
      std::vector<std::int64_t> cell;
      for (std::uint64_t k = 0; k < n; ++k) {
        const std::uint64_t id = file_index(data[at++]);
        cell.push_back(id == invalid_index ? -1 : static_cast<std::int64_t>(id));
      }
      out.push_back(std::move(cell));
    }
    return out;
  }

  Result<ir::Mesh> read() {
    const std::string_view header = in_.raw_line();
    if (!header.starts_with("# vtk DataFile")) return fail(Status::Decode, "not a VTK legacy file");
    const std::string title(util::trim(in_.raw_line()));
    const std::string_view encoding = util::trim(in_.raw_line());
    if (util::iequals(encoding, "BINARY")) binary_ = true;
    else if (!util::iequals(encoding, "ASCII")) return fail(Status::Decode, "unknown VTK encoding '{}'", encoding);
    ir::Mesh mesh;
    if (!title.empty() && title != "vtk output") mesh.metadata["title"] = title;
    std::vector<std::vector<std::int64_t>> polygons, strips, vertices;
    std::vector<std::vector<std::int64_t>> grid_cells;
    std::vector<double> cell_types;
    std::uint64_t point_count = 0;
    Data data = Data::None;
    std::uint64_t data_count = 0;
    std::size_t skipped_volume = 0;
    while (!in_.at_end()) {
      const std::string_view key = in_.token();
      if (key.empty()) break;
      if ((++sections_ & 0xFF) == 0) CVT_TRY(ctx_.checkpoint(0.8 * static_cast<double>(in_.position()) / (ctx_.input.size() + 1.0)));
      const std::string rest(in_.line());
      std::vector<std::string_view> args = tokens(rest);
      const auto number = [&](std::size_t i) -> std::uint64_t {
        std::int64_t v = 0;
        return i < args.size() && parse_int(args[i], v) && v >= 0 ? static_cast<std::uint64_t>(v) : 0;
      };
      if (key == "DATASET") {
        if (args.empty() || (args[0] != "POLYDATA" && args[0] != "UNSTRUCTURED_GRID")) {
          return fail(Status::Decode, "VTK dataset '{}' is not supported (POLYDATA and UNSTRUCTURED_GRID are)", args.empty() ? "" : args[0]);
        }
      } else if (key == "POINTS") {
        point_count = number(0);
        CVT_TRY_ASSIGN(const std::vector<double> v, values(point_count * 3, args.size() > 1 ? args[1] : "float"));
        for (std::size_t i = 0; i + 2 < v.size(); i += 3) {
          mesh.positions.push_back({static_cast<float>(v[i]), static_cast<float>(v[i + 1]), static_cast<float>(v[i + 2])});
        }
      } else if (key == "POLYGONS" || key == "TRIANGLE_STRIPS" || key == "VERTICES" || key == "LINES" || key == "CELLS") {
        CVT_TRY_ASSIGN(auto list, cells(number(0), number(1)));
        if (key == "POLYGONS") polygons = std::move(list);
        else if (key == "TRIANGLE_STRIPS") strips = std::move(list);
        else if (key == "VERTICES") vertices = std::move(list);
        else if (key == "CELLS") grid_cells = std::move(list);
        else ctx_.warn("VTK lines were not converted.");
      } else if (key == "CELL_TYPES") {
        CVT_TRY_ASSIGN(cell_types, values(number(0), "int"));
      } else if (key == "POINT_DATA") {
        data = Data::Point;
        data_count = number(0);
      } else if (key == "CELL_DATA") {
        data = Data::Cell;
        data_count = number(0);
      } else if (key == "SCALARS" || key == "COLOR_SCALARS" || key == "NORMALS" || key == "TEXTURE_COORDINATES" ||
                 key == "VECTORS" || key == "TENSORS" || key == "LOOKUP_TABLE" || key == "FIELD") {
        CVT_TRY(attribute(key, args, data, data_count, mesh));
      } else if (key == "METADATA") {
        while (!in_.at_end() && !util::trim(in_.raw_line()).empty()) {
        }
      } else if (key == "FIELD_DATA" || key == "SPACING" || key == "ORIGIN" || key == "DIMENSIONS") {
        // dataset-level extras we do not need
      } else {
        return fail(Status::Decode, "unexpected '{}' in the VTK file", key);
      }
    }
    const auto add_face = [&](const std::vector<std::int64_t>& cell) -> Result<void> {
      for (const std::int64_t v : cell) {
        if (v < 0 || static_cast<std::uint64_t>(v) >= mesh.positions.size()) return fail(Status::Decode, "a VTK cell refers to missing point {}", v);
        mesh.indices.push_back(static_cast<std::uint32_t>(v));
      }
      mesh.face_sizes.push_back(static_cast<std::uint32_t>(cell.size()));
      return {};
    };
    for (const auto& cell : polygons) {
      if (cell.size() >= 3) CVT_TRY(add_face(cell));
    }
    for (const auto& strip : strips) {
      for (std::size_t i = 2; i < strip.size(); ++i) {
        std::vector<std::int64_t> tri = i % 2 == 0 ? std::vector<std::int64_t>{strip[i - 2], strip[i - 1], strip[i]}
                                                   : std::vector<std::int64_t>{strip[i - 1], strip[i - 2], strip[i]};
        CVT_TRY(add_face(tri));
      }
    }
    for (std::size_t c = 0; c < grid_cells.size(); ++c) {
      const std::uint64_t raw_type = c < cell_types.size() ? file_index(cell_types[c]) : 0;
      const int type = raw_type < 100 ? static_cast<int>(raw_type) : 0;
      const auto& cell = grid_cells[c];
      if (type == 5 || type == 7 || type == 9) {
        if (cell.size() >= 3) CVT_TRY(add_face(cell));
      } else if (type == 8 && cell.size() == 4) {  // pixel: corner order 0 1 3 2
        CVT_TRY(add_face({cell[0], cell[1], cell[3], cell[2]}));
      } else if (type == 6) {
        for (std::size_t i = 2; i < cell.size(); ++i) {
          CVT_TRY(add_face(i % 2 == 0 ? std::vector<std::int64_t>{cell[i - 2], cell[i - 1], cell[i]}
                                      : std::vector<std::int64_t>{cell[i - 1], cell[i - 2], cell[i]}));
        }
      } else if (type >= 10) {
        ++skipped_volume;
      }
    }
    if (skipped_volume > 0) ctx_.warn(std::format("{} volume cells were skipped (only surface cells are converted).", skipped_volume));
    (void)vertices;
    (void)point_count;
    return mesh;
  }

 private:
  Result<void> attribute(std::string_view key, const std::vector<std::string_view>& args, Data data, std::uint64_t count,
                         ir::Mesh& mesh) {
    const bool keep = data == Data::Point && count == mesh.positions.size();
    const auto arg = [&](std::size_t i, std::string_view fallback) { return i < args.size() ? args[i] : fallback; };
    const auto number = [&](std::size_t i, std::uint64_t fallback) -> std::uint64_t {
      std::int64_t v = 0;
      return i < args.size() && parse_int(args[i], v) && v >= 0 ? static_cast<std::uint64_t>(v) : fallback;
    };
    if (key == "SCALARS") {
      const std::uint64_t components = std::clamp<std::uint64_t>(number(2, 1), 1, 4);
      if (in_.peek_token() == "LOOKUP_TABLE") {
        in_.token();
        (void)in_.line();
      }
      CVT_TRY_ASSIGN(const std::vector<double> v, values(count * components, arg(1, "float")));
      if (keep && components == 1) {
        ir::VertexAttribute a{std::string(arg(0, "scalars")), {}};
        for (const double x : v) a.values.push_back(static_cast<float>(x));
        mesh.attributes.push_back(std::move(a));
      }
    } else if (key == "COLOR_SCALARS") {
      const std::uint64_t components = std::clamp<std::uint64_t>(number(1, 4), 1, 4);
      CVT_TRY_ASSIGN(const std::vector<double> v, values(count * components, binary_ ? "unsigned_char" : "float"));
      if (keep && components >= 3) {
        const double scale = binary_ ? 255.0 : 1.0;
        for (std::size_t i = 0; i < count; ++i) {
          const double* c = v.data() + i * components;
          mesh.colors.push_back(color_from_srgb(c[0], c[1], c[2], components == 4 ? c[3] : -1.0, scale));
        }
      }
    } else if (key == "NORMALS") {
      CVT_TRY_ASSIGN(const std::vector<double> v, values(count * 3, arg(1, "float")));
      if (keep) {
        for (std::size_t i = 0; i + 2 < v.size(); i += 3) {
          mesh.normals.push_back({static_cast<float>(v[i]), static_cast<float>(v[i + 1]), static_cast<float>(v[i + 2])});
        }
      }
    } else if (key == "TEXTURE_COORDINATES") {
      const std::uint64_t dimension = std::clamp<std::uint64_t>(number(1, 2), 1, 3);
      CVT_TRY_ASSIGN(const std::vector<double> v, values(count * dimension, arg(2, "float")));
      if (keep && dimension >= 2) {
        for (std::size_t i = 0; i < count; ++i) {
          mesh.texcoords.push_back({static_cast<float>(v[i * dimension]), static_cast<float>(v[i * dimension + 1])});
        }
      }
    } else if (key == "VECTORS") {
      CVT_TRY(values(count * 3, arg(1, "float")));
    } else if (key == "TENSORS") {
      CVT_TRY(values(count * 9, arg(1, "float")));
    } else if (key == "LOOKUP_TABLE") {
      CVT_TRY(values(number(1, 0) * 4, binary_ ? "unsigned_char" : "float"));
    } else if (key == "FIELD") {
      const std::uint64_t arrays = number(1, 0);
      for (std::uint64_t k = 0; k < arrays; ++k) {
        std::string header(in_.line());
        if (util::trim(header) == "METADATA") {
          while (!in_.at_end() && !util::trim(in_.raw_line()).empty()) {
          }
          header = std::string(in_.line());
        }
        const std::vector<std::string_view> parts = tokens(header);
        if (parts.size() < 4) return fail(Status::Decode, "malformed VTK field array");
        std::int64_t components = 0, tuples = 0;
        if (!parse_int(parts[1], components) || !parse_int(parts[2], tuples) || components < 1 || tuples < 0 || components > 64) {
          return fail(Status::Decode, "malformed VTK field array");
        }
        CVT_TRY_ASSIGN(const std::vector<double> v,
                       values(static_cast<std::uint64_t>(components) * static_cast<std::uint64_t>(tuples), parts[3]));
        if (keep && components == 1 && static_cast<std::uint64_t>(tuples) == count) {
          ir::VertexAttribute a{std::string(parts[0]), {}};
          for (const double x : v) a.values.push_back(static_cast<float>(x));
          mesh.attributes.push_back(std::move(a));
        }
      }
    }
    return {};
  }

  ReadContext& ctx_;
  Cursor in_;
  bool binary_ = false;
  std::size_t sections_ = 0;
};


Result<AnyIR> read_vtk(ReadContext& ctx) {
  VtkReader reader(ctx);
  CVT_TRY_ASSIGN(ir::Mesh mesh, reader.read());
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

constexpr WriterTraits vtk_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.point_clouds = true;
  traits.materials = false;
  return traits;
}

Result<Bytes> write_vtk(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, vtk_traits()));
  const bool binary = ctx.options.get_bool("binary", false);
  const int precision = write_precision(ctx);
  std::string title = "vtk output";
  if (!options::resolved_strip_metadata(ctx.options) && !mesh.name.empty()) {
    title.clear();
    for (const char c : mesh.name) title.push_back(c == '\n' || c == '\r' ? ' ' : c);
  }
  util::ByteWriter out(mesh.positions.size() * 40 + mesh.indices.size() * 8 + 256);
  out.text(std::format("# vtk DataFile Version 3.0\n{}\n{}\nDATASET POLYDATA\n", title, binary ? "BINARY" : "ASCII"));
  const std::size_t n = mesh.positions.size();
  const auto floats = [&](const std::vector<float>& values, std::size_t per_line) {
    if (binary) {
      for (const float v : values) out.f32be(v);
      out.text("\n");
      return;
    }
    std::string line;
    for (std::size_t i = 0; i < values.size(); ++i) {
      append_number(line, values[i], precision);
      line += (i + 1) % per_line == 0 ? '\n' : ' ';
      if (line.size() > 4096) {
        out.text(line);
        line.clear();
      }
    }
    out.text(line);
  };
  std::vector<float> values;
  values.reserve(n * 3);
  for (const ir::Vec3& p : mesh.positions) values.insert(values.end(), {p.x, p.y, p.z});
  out.text(std::format("POINTS {} float\n", n));
  floats(values, 3);
  const auto cell_list = [&](std::string_view name, std::size_t cells, std::size_t size, auto&& each) {
    out.text(std::format("{} {} {}\n", name, cells, size));
    std::string line;
    each([&](std::uint32_t value, bool end_of_cell) {
      if (binary) {
        out.i32be(static_cast<std::int32_t>(value));
        return;
      }
      line += std::to_string(value);
      line += end_of_cell ? '\n' : ' ';
      if (line.size() > 4096) {
        out.text(line);
        line.clear();
      }
    });
    out.text(line);
    if (binary) out.text("\n");
  };
  if (mesh.point_cloud) {
    cell_list("VERTICES", n, n * 2, [&](auto&& emit) {
      for (std::size_t i = 0; i < n; ++i) {
        emit(1, false);
        emit(static_cast<std::uint32_t>(i), true);
      }
    });
  } else {
    const std::vector<std::size_t> offsets = face_offsets(mesh);
    const std::size_t faces = offsets.size() - 1;
    cell_list("POLYGONS", faces, faces + mesh.indices.size(), [&](auto&& emit) {
      for (std::size_t f = 0; f < faces; ++f) {
        emit(static_cast<std::uint32_t>(offsets[f + 1] - offsets[f]), false);
        for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) emit(mesh.indices[c], c + 1 == offsets[f + 1]);
      }
    });
  }
  if (!mesh.normals.empty() || !mesh.texcoords.empty() || !mesh.colors.empty() || !mesh.attributes.empty()) {
    out.text(std::format("POINT_DATA {}\n", n));
  }
  if (!mesh.normals.empty()) {
    values.clear();
    for (const ir::Vec3& v : mesh.normals) values.insert(values.end(), {v.x, v.y, v.z});
    out.text("NORMALS normals float\n");
    floats(values, 3);
  }
  if (!mesh.texcoords.empty()) {
    values.clear();
    for (const ir::Vec2& v : mesh.texcoords) values.insert(values.end(), {v.x, v.y});
    out.text("TEXTURE_COORDINATES tcoords 2 float\n");
    floats(values, 2);
  }
  if (!mesh.colors.empty()) {
    out.text("COLOR_SCALARS colors 4\n");
    if (binary) {
      for (const ir::Color& c : mesh.colors) {
        const auto b = color_to_srgb8(c);
        out.u8(b[0]).u8(b[1]).u8(b[2]).u8(b[3]);
      }
      out.text("\n");
    } else {
      values.clear();
      for (const ir::Color& c : mesh.colors) {
        const auto b = color_to_srgb8(c);
        for (const std::uint8_t x : b) values.push_back(static_cast<float>(x) / 255.0f);
      }
      floats(values, 4);
    }
  }
  for (const ir::VertexAttribute& a : mesh.attributes) {
    std::string name;
    for (const char c : a.name) name.push_back(util::is_ascii_space(c) ? '_' : c);
    out.text(std::format("SCALARS {} float 1\nLOOKUP_TABLE default\n", name.empty() ? "scalars" : name));
    floats(a.values, 1);
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out.take();
}

int sniff_vtk(ByteView data, std::string_view /*filename*/) {
  return text_head(data, 64).starts_with("# vtk DataFile Version") ? 98 : 0;
}

// --------------------------------------------------------------------------- Gmsh

// Corner nodes used for the surface element types we convert.
int gmsh_corners(std::int64_t type) {
  switch (type) {
    case 2:   // 3-node triangle
    case 9:   // 6-node triangle
    case 20:  // 9-node triangle
    case 21:  // 10-node triangle
      return 3;
    case 3:   // 4-node quadrangle
    case 10:  // 9-node quadrangle
    case 16:  // 8-node quadrangle
      return 4;
    default: return 0;
  }
}

// Total node count of an element type (for binary v2 records).
int gmsh_nodes(std::int64_t type) {
  static const std::map<std::int64_t, int> nodes = {{1, 2},  {2, 3},  {3, 4},  {4, 4},  {5, 8},  {6, 6},  {7, 5},
                                                    {8, 3},  {9, 6},  {10, 9}, {11, 10}, {12, 27}, {13, 18}, {14, 14},
                                                    {15, 1}, {16, 8}, {17, 20}, {18, 15}, {19, 13}, {20, 9}, {21, 10}};
  const auto it = nodes.find(type);
  return it == nodes.end() ? -1 : it->second;
}

Result<AnyIR> read_gmsh(ReadContext& ctx) {
  Cursor in(ctx.input);
  ir::Mesh mesh;
  std::unordered_map<std::int64_t, std::uint32_t> node_index;
  std::map<std::int64_t, std::string> physical_names;
  double version = 0;
  bool binary = false;
  bool swap = false;
  std::size_t volumes = 0;
  // element runs per group tag (physical in v2, entity in v4)
  std::map<std::int64_t, std::vector<std::vector<std::uint32_t>>> faces_by_tag;
  const auto add_face = [&](std::int64_t tag, std::int64_t type, const std::vector<std::int64_t>& nodes) -> Result<void> {
    const int corners = gmsh_corners(type);
    if (corners == 0) {
      if (type == 4 || type == 5 || type == 6 || type == 7 || type == 11 || type == 12 || type == 17) ++volumes;
      return {};
    }
    if (nodes.size() < static_cast<std::size_t>(corners)) return fail(Status::Decode, "a Gmsh element has too few nodes");
    std::vector<std::uint32_t> face;
    for (int k = 0; k < corners; ++k) {
      const auto it = node_index.find(nodes[static_cast<std::size_t>(k)]);
      if (it == node_index.end()) return fail(Status::Decode, "a Gmsh element refers to missing node {}", nodes[static_cast<std::size_t>(k)]);
      face.push_back(it->second);
    }
    faces_by_tag[tag].push_back(std::move(face));
    return {};
  };
  const auto integer = [&](std::int64_t& out) {
    return parse_int(in.token(), out);
  };
  const auto read_i32 = [&]() -> std::int64_t {
    util::ByteReader r(in.bytes(4));
    const std::uint32_t v = r.u32le();
    return static_cast<std::int32_t>(swap ? std::byteswap(v) : v);
  };
  const auto read_f64 = [&]() -> double {
    util::ByteReader r(in.bytes(8));
    const std::uint64_t v = r.u64le();
    return std::bit_cast<double>(swap ? std::byteswap(v) : v);
  };
  while (!in.at_end()) {
    const std::string_view section = in.token();
    if (section.empty()) break;
    CVT_TRY(ctx.checkpoint(0.8 * static_cast<double>(in.position()) / (ctx.input.size() + 1.0)));
    if (section == "$MeshFormat") {
      if (!parse_double(in.token(), version)) return fail(Status::Decode, "malformed Gmsh header");
      std::int64_t file_type = 0, size = 0;
      if (!integer(file_type) || !integer(size)) return fail(Status::Decode, "malformed Gmsh header");
      binary = file_type == 1;
      if (binary) {
        (void)in.raw_line();
        const std::int64_t one = read_i32();
        if (one != 1) {
          swap = true;
          if (std::byteswap(static_cast<std::uint32_t>(one)) != 1) return fail(Status::Decode, "malformed Gmsh binary header");
        }
        if (version >= 4) return fail(Status::Decode, "binary Gmsh 4 files are not supported; save as ASCII or format 2.2");
      }
      if (version < 2 || version >= 5) return fail(Status::Decode, "Gmsh format {} is not supported", version);
    } else if (section == "$PhysicalNames") {
      std::int64_t count = 0;
      if (!integer(count) || count < 0) return fail(Status::Decode, "malformed Gmsh physical names");
      for (std::int64_t i = 0; i < count; ++i) {
        std::int64_t dim = 0, tag = 0;
        if (!integer(dim) || !integer(tag)) return fail(Status::Decode, "malformed Gmsh physical names");
        std::string name(util::trim(in.line()));
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
        physical_names[tag] = std::move(name);
      }
    } else if (section == "$Nodes") {
      if (version < 4) {
        std::int64_t count = 0;
        if (!integer(count) || count < 0) return fail(Status::Decode, "malformed Gmsh nodes");
        CVT_TRY(check_records(ctx, static_cast<std::uint64_t>(count), binary ? 28 : 8, in.remaining(), "nodes"));
        if (binary) (void)in.raw_line();
        for (std::int64_t i = 0; i < count; ++i) {
          std::int64_t id = 0;
          double xyz[3] = {0, 0, 0};
          if (binary) {
            id = read_i32();
            for (double& v : xyz) v = read_f64();
          } else if (!integer(id) || !parse_double(in.token(), xyz[0]) || !parse_double(in.token(), xyz[1]) ||
                     !parse_double(in.token(), xyz[2])) {
            return fail(Status::Decode, "malformed Gmsh node");
          }
          node_index[id] = static_cast<std::uint32_t>(mesh.positions.size());
          mesh.positions.push_back({static_cast<float>(xyz[0]), static_cast<float>(xyz[1]), static_cast<float>(xyz[2])});
        }
      } else {
        std::int64_t blocks = 0, count = 0, min_tag = 0, max_tag = 0;
        if (!integer(blocks) || !integer(count) || !integer(min_tag) || !integer(max_tag) || blocks < 0 || count < 0) {
          return fail(Status::Decode, "malformed Gmsh nodes");
        }
        CVT_TRY(check_records(ctx, static_cast<std::uint64_t>(count), 8, in.remaining(), "nodes"));
        for (std::int64_t b = 0; b < blocks; ++b) {
          std::int64_t dim = 0, entity = 0, parametric = 0, n = 0;
          if (!integer(dim) || !integer(entity) || !integer(parametric) || !integer(n) || n < 0 || n > count) {
            return fail(Status::Decode, "malformed Gmsh node block");
          }
          std::vector<std::int64_t> tags(static_cast<std::size_t>(n));
          for (std::int64_t& t : tags) {
            if (!integer(t)) return fail(Status::Decode, "malformed Gmsh node block");
          }
          for (const std::int64_t t : tags) {
            double xyz[3] = {0, 0, 0};
            for (double& v : xyz) {
              if (!parse_double(in.token(), v)) return fail(Status::Decode, "malformed Gmsh node");
            }
            if (parametric != 0) {
              for (std::int64_t k = 0; k < dim; ++k) (void)in.token();
            }
            node_index[t] = static_cast<std::uint32_t>(mesh.positions.size());
            mesh.positions.push_back({static_cast<float>(xyz[0]), static_cast<float>(xyz[1]), static_cast<float>(xyz[2])});
          }
        }
      }
    } else if (section == "$Elements") {
      if (version < 4) {
        std::int64_t count = 0;
        if (!integer(count) || count < 0) return fail(Status::Decode, "malformed Gmsh elements");
        CVT_TRY(check_records(ctx, static_cast<std::uint64_t>(count), binary ? 16 : 8, in.remaining(), "elements"));
        if (binary) {
          (void)in.raw_line();
          std::int64_t done = 0;
          while (done < count) {
            const std::int64_t type = read_i32(), n = read_i32(), ntags = read_i32();
            const int nodes = gmsh_nodes(type);
            if (nodes < 0 || n <= 0 || ntags < 0 || ntags > 64 || n > count - done) return fail(Status::Decode, "malformed Gmsh binary elements");
            for (std::int64_t e = 0; e < n; ++e) {
              (void)read_i32();  // id
              std::int64_t physical = 0;
              for (std::int64_t t = 0; t < ntags; ++t) {
                const std::int64_t tag = read_i32();
                if (t == 0) physical = tag;
              }
              std::vector<std::int64_t> ids(static_cast<std::size_t>(nodes));
              for (std::int64_t& id : ids) id = read_i32();
              if (in.at_end() && e + 1 < n) return fail(Status::Decode, "the Gmsh file is truncated");
              CVT_TRY(add_face(physical, type, ids));
            }
            done += n;
          }
        } else {
          for (std::int64_t i = 0; i < count; ++i) {
            std::int64_t id = 0, type = 0, ntags = 0;
            if (!integer(id) || !integer(type) || !integer(ntags) || ntags < 0 || ntags > 64) return fail(Status::Decode, "malformed Gmsh element");
            std::int64_t physical = 0;
            for (std::int64_t t = 0; t < ntags; ++t) {
              std::int64_t tag = 0;
              if (!integer(tag)) return fail(Status::Decode, "malformed Gmsh element");
              if (t == 0) physical = tag;
            }
            const int nodes = gmsh_nodes(type);
            if (nodes < 0) return fail(Status::Decode, "unknown Gmsh element type {}", type);
            std::vector<std::int64_t> ids(static_cast<std::size_t>(nodes));
            for (std::int64_t& n : ids) {
              if (!integer(n)) return fail(Status::Decode, "malformed Gmsh element");
            }
            CVT_TRY(add_face(physical, type, ids));
          }
        }
      } else {
        std::int64_t blocks = 0, count = 0, min_tag = 0, max_tag = 0;
        if (!integer(blocks) || !integer(count) || !integer(min_tag) || !integer(max_tag) || blocks < 0 || count < 0) {
          return fail(Status::Decode, "malformed Gmsh elements");
        }
        CVT_TRY(check_records(ctx, static_cast<std::uint64_t>(count), 4, in.remaining(), "elements"));
        for (std::int64_t b = 0; b < blocks; ++b) {
          std::int64_t dim = 0, entity = 0, type = 0, n = 0;
          if (!integer(dim) || !integer(entity) || !integer(type) || !integer(n) || n < 0 || n > count) {
            return fail(Status::Decode, "malformed Gmsh element block");
          }
          const int nodes = gmsh_nodes(type);
          if (nodes < 0) return fail(Status::Decode, "unknown Gmsh element type {}", type);
          for (std::int64_t e = 0; e < n; ++e) {
            std::int64_t id = 0;
            if (!integer(id)) return fail(Status::Decode, "malformed Gmsh element");
            std::vector<std::int64_t> ids(static_cast<std::size_t>(nodes));
            for (std::int64_t& node : ids) {
              if (!integer(node)) return fail(Status::Decode, "malformed Gmsh element");
            }
            CVT_TRY(add_face(dim == 2 ? entity : -1, type, ids));
          }
        }
      }
    }
    // Skip to the end marker of this section.
    if (section.starts_with("$") && !section.starts_with("$End")) {
      const std::string end = "$End" + std::string(section.substr(1));
      for (;;) {
        const std::string_view t = in.token();
        if (t.empty() || t == end) break;
      }
    }
  }
  if (version == 0) return fail(Status::Decode, "not a Gmsh mesh file");
  for (auto& [tag, faces] : faces_by_tag) {
    const std::size_t first = mesh.face_count();
    for (const auto& face : faces) {
      mesh.indices.insert(mesh.indices.end(), face.begin(), face.end());
      mesh.face_sizes.push_back(static_cast<std::uint32_t>(face.size()));
    }
    const auto name = physical_names.find(tag);
    mesh.groups.push_back({name != physical_names.end() ? name->second : std::format("surface_{}", tag), "", first, faces.size()});
  }
  if (mesh.groups.size() == 1 && mesh.groups[0].name.starts_with("surface_")) mesh.groups.clear();
  if (volumes > 0) ctx.warn(std::format("{} volume elements were skipped (only surface elements are converted).", volumes));
  remove_unused_vertices(mesh);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

int sniff_gmsh(ByteView data, std::string_view /*filename*/) {
  return text_head(data, 64).starts_with("$MeshFormat") ? 97 : 0;
}

}  // namespace

void register_vtk(Registry& registry) {
  FormatInfo info;
  info.id = "vtk";
  info.name = "VTK legacy file";
  info.description = "Visualization Toolkit legacy polygon data (ASCII or binary).";
  info.category = Category::Model3D;
  info.extensions = {"vtk"};
  info.mime = "application/x-vtk";
  info.popularity = 20;
  info.sniff = sniff_vtk;
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = "vtk";
  codec.kind = Kind::Mesh;
  codec.read = read_vtk;
  codec.write = write_vtk;
  codec.write_options = mesh_write_options(vtk_traits());
  codec.write_options.insert(codec.write_options.begin(),
                             OptionSpec::boolean("binary", "Binary", false, "Write big-endian binary data instead of text."));
  registry.add_codec(std::move(codec));
}

void register_gmsh(Registry& registry) {
  FormatInfo info;
  info.id = "msh";
  info.name = "Gmsh mesh";
  info.description = "Finite-element mesh from Gmsh (format 2.2 and 4.1); surface elements are converted.";
  info.category = Category::Model3D;
  info.extensions = {"msh"};
  info.mime = "application/x-gmsh";
  info.popularity = 10;
  info.sniff = sniff_gmsh;
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = "msh";
  codec.kind = Kind::Mesh;
  codec.read = read_gmsh;
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::mesh
