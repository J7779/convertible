// Convertible core - mesh family: point clouds (XYZ/PTS text lists, PCD, LAS).
//
//   xyz  text rows "x y z [r g b] [nx ny nz] [intensity]" in any of the
//        common layouts (CloudCompare, Open3D .xyzn/.xyzrgb, Leica .pts)
//   pcd  Point Cloud Library files: ascii, binary and binary_compressed
//        (column-major LZF; compressor and decompressor written here)
//   las  ASPRS LAS 1.0-1.4, point data record formats 0-10 (LAZ is not
//        supported)
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

// --------------------------------------------------------------------------- LZF (also used by tests)

/// LZF decompression (liblzf format). Fails when the output would differ
/// from `expected` bytes or a back reference points before the start.
Result<Bytes> lzf_decompress(ByteView input, std::size_t expected) {
  Bytes out;
  out.reserve(expected);
  const auto* in = as_u8(input).data();
  std::size_t i = 0;
  const std::size_t n = input.size();
  while (i < n) {
    const std::uint32_t control = in[i++];
    if (control < 32) {
      const std::size_t run = control + 1;
      if (run > n - i || out.size() + run > expected) return fail(Status::Decode, "corrupt LZF data");
      out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(i), input.begin() + static_cast<std::ptrdiff_t>(i + run));
      i += run;
    } else {
      std::size_t length = control >> 5;
      if (length == 7) {
        if (i >= n) return fail(Status::Decode, "corrupt LZF data");
        length += in[i++];
      }
      if (i >= n) return fail(Status::Decode, "corrupt LZF data");
      const std::size_t back = ((control & 0x1F) << 8) + in[i++] + 1;
      length += 2;
      if (back > out.size() || out.size() + length > expected) return fail(Status::Decode, "corrupt LZF data");
      const std::size_t from = out.size() - back;
      for (std::size_t k = 0; k < length; ++k) out.push_back(out[from + k]);  // may overlap
    }
  }
  if (out.size() != expected) return fail(Status::Decode, "LZF data is truncated");
  return out;
}

/// LZF compression (liblzf format, deterministic hash-chain-free variant).
Bytes lzf_compress(ByteView input) {
  const auto* in = as_u8(input).data();
  const std::size_t n = input.size();
  Bytes out;
  out.reserve(n + n / 16 + 64);
  constexpr std::size_t hash_bits = 14;
  std::vector<std::size_t> table(std::size_t{1} << hash_bits, std::numeric_limits<std::size_t>::max());
  std::vector<std::uint8_t> literal;
  const auto flush = [&]() {
    std::size_t at = 0;
    while (at < literal.size()) {
      const std::size_t run = std::min<std::size_t>(32, literal.size() - at);
      out.push_back(static_cast<std::byte>(run - 1));
      for (std::size_t k = 0; k < run; ++k) out.push_back(static_cast<std::byte>(literal[at + k]));
      at += run;
    }
    literal.clear();
  };
  std::size_t i = 0;
  while (i < n) {
    if (i + 2 < n) {
      const std::uint32_t key = (static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8) | in[i + 2];
      const std::size_t slot = (key * 2654435761u) >> (32 - hash_bits);
      const std::size_t candidate = table[slot];
      table[slot] = i;
      if (candidate != std::numeric_limits<std::size_t>::max() && i - candidate <= 8192 && i > candidate &&
          ::cvt::util::compare_bytes(in + candidate, in + i, 3) == 0) {
        std::size_t length = 3;
        const std::size_t max_length = std::min<std::size_t>(264, n - i);
        while (length < max_length && in[candidate + length] == in[i + length]) ++length;
        flush();
        const std::size_t back = i - candidate - 1;
        const std::size_t code = length - 2;
        if (code < 7) {
          out.push_back(static_cast<std::byte>((code << 5) | (back >> 8)));
        } else {
          out.push_back(static_cast<std::byte>((7 << 5) | (back >> 8)));
          out.push_back(static_cast<std::byte>(code - 7));
        }
        out.push_back(static_cast<std::byte>(back & 0xFF));
        i += length;
        continue;
      }
    }
    literal.push_back(in[i++]);
  }
  flush();
  return out;
}

namespace {

ir::Mesh empty_cloud() {
  ir::Mesh m;
  m.point_cloud = true;
  return m;
}

ir::VertexAttribute& attribute(ir::Mesh& mesh, std::string_view name) {
  for (ir::VertexAttribute& a : mesh.attributes) {
    if (a.name == name) return a;
  }
  mesh.attributes.push_back({std::string(name), {}});
  return mesh.attributes.back();
}

constexpr WriterTraits points_traits() {
  WriterTraits traits;
  traits.points_only = true;
  traits.texcoords = false;
  traits.materials = false;
  traits.text = true;
  return traits;
}

// --------------------------------------------------------------------------- XYZ / PTS

bool comment_line(std::string_view line) {
  line = util::trim(line);
  return line.empty() || line.starts_with('#') || line.starts_with("//");
}

enum class XyzLayout { Xyz, XyzI, XyzRgb, XyzRgbFloat, XyzN, XyzIRgb, XyzRgbN, XyzNRgb };

Result<AnyIR> read_xyz(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  if (!looks_textual(ctx.input)) return fail(Status::Decode, "not a point list");
  // First pass: the layout from the data rows.
  LineReader probe(text);
  std::string_view line;
  std::size_t columns = 0;
  std::vector<std::vector<double>> sample;
  while (probe.next(line) && sample.size() < 64) {
    if (comment_line(line)) continue;
    std::vector<std::string_view> parts = tokens(line, ",;");
    if (parts.size() == 1 && sample.empty()) continue;  // Leica .pts point count
    std::vector<double> row;
    for (const std::string_view p : parts) {
      double v = 0;
      if (!parse_double(p, v)) return fail(Status::Decode, "line {} is not a row of numbers", probe.line_number());
      row.push_back(v);
    }
    if (columns == 0) columns = row.size();
    if (row.size() == columns) sample.push_back(std::move(row));
  }
  if (columns < 3) return fail(Status::Decode, "a point list needs at least three columns");
  const auto all = [&](std::size_t from, std::size_t to, auto&& predicate) {
    for (const auto& row : sample) {
      for (std::size_t c = from; c < to && c < row.size(); ++c) {
        if (!predicate(row[c])) return false;
      }
    }
    return true;
  };
  const auto bytes = [](double v) { return v >= 0 && v <= 255 && v == std::floor(v); };
  const auto unit_range = [](double v) { return v >= 0 && v <= 1; };
  const auto unit_vectors = [&](std::size_t from) {
    for (const auto& row : sample) {
      const double l = row[from] * row[from] + row[from + 1] * row[from + 1] + row[from + 2] * row[from + 2];
      if (std::abs(l - 1.0) > 1e-3) return false;
    }
    return true;
  };
  XyzLayout layout = XyzLayout::Xyz;
  const std::string& name = ctx.filename;
  if (columns == 4) layout = XyzLayout::XyzI;
  else if (columns == 6) {
    if (has_extension(name, "xyzn")) layout = XyzLayout::XyzN;
    else if (has_extension(name, "xyzrgb")) layout = all(3, 6, unit_range) ? XyzLayout::XyzRgbFloat : XyzLayout::XyzRgb;
    else if (all(3, 6, bytes) && !all(3, 6, unit_range)) layout = XyzLayout::XyzRgb;
    else if (unit_vectors(3)) layout = XyzLayout::XyzN;
    else if (all(3, 6, unit_range)) layout = XyzLayout::XyzRgbFloat;
    else layout = XyzLayout::XyzN;
  } else if (columns == 7) {
    layout = XyzLayout::XyzIRgb;  // Leica .pts
  } else if (columns >= 9) {
    layout = all(3, 6, bytes) && !unit_vectors(3) ? XyzLayout::XyzRgbN : XyzLayout::XyzNRgb;
  }

  ir::Mesh mesh = empty_cloud();
  LineReader lines(text);
  std::vector<double> row;
  bool first = true;
  while (lines.next(line)) {
    if ((lines.line_number() & 0x3FFF) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(lines.position()) / (static_cast<double>(text.size()) + 1.0)));
    }
    if (comment_line(line)) continue;
    std::vector<std::string_view> parts = tokens(line, ",;");
    if (parts.size() == 1 && (first || has_extension(name, "pts"))) {
      first = false;
      continue;  // a count line
    }
    first = false;
    if (parts.size() != columns) return fail(Status::Decode, "line {} has {} columns instead of {}", lines.line_number(), parts.size(), columns);
    row.clear();
    for (const std::string_view p : parts) {
      double v = 0;
      if (!parse_double(p, v)) return fail(Status::Decode, "line {} is not a row of numbers", lines.line_number());
      row.push_back(v);
    }
    mesh.positions.push_back({static_cast<float>(row[0]), static_cast<float>(row[1]), static_cast<float>(row[2])});
    if ((mesh.positions.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx, mesh.positions.size(), "points"));
    const auto f = [&](std::size_t i) { return static_cast<float>(row[i]); };
    switch (layout) {
      case XyzLayout::Xyz: break;
      case XyzLayout::XyzI: attribute(mesh, "intensity").values.push_back(f(3)); break;
      case XyzLayout::XyzRgb: mesh.colors.push_back(color_from_srgb(row[3], row[4], row[5])); break;
      case XyzLayout::XyzRgbFloat: mesh.colors.push_back(color_from_srgb(row[3], row[4], row[5], -1.0, 1.0)); break;
      case XyzLayout::XyzN: mesh.normals.push_back({f(3), f(4), f(5)}); break;
      case XyzLayout::XyzIRgb:
        attribute(mesh, "intensity").values.push_back(f(3));
        mesh.colors.push_back(color_from_srgb(row[4], row[5], row[6]));
        break;
      case XyzLayout::XyzRgbN:
        mesh.colors.push_back(color_from_srgb(row[3], row[4], row[5]));
        mesh.normals.push_back({f(6), f(7), f(8)});
        break;
      case XyzLayout::XyzNRgb:
        mesh.normals.push_back({f(3), f(4), f(5)});
        mesh.colors.push_back(color_from_srgb(row[6], row[7], row[8], -1.0, all(6, 9, unit_range) ? 1.0 : 255.0));
        break;
    }
  }
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

Result<Bytes> write_xyz(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, points_traits()));
  const int precision = write_precision(ctx);
  std::string out;
  out.reserve(mesh.positions.size() * 48);
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    if ((i & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(0.8 + 0.2 * static_cast<double>(i) / static_cast<double>(mesh.positions.size())));
    const ir::Vec3& p = mesh.positions[i];
    append_number(out, p.x, precision);
    out += ' ';
    append_number(out, p.y, precision);
    out += ' ';
    append_number(out, p.z, precision);
    if (!mesh.colors.empty()) {
      const auto c = color_to_srgb8(mesh.colors[i]);
      out += std::format(" {} {} {}", c[0], c[1], c[2]);
    }
    if (!mesh.normals.empty()) {
      for (const float v : {mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z}) {
        out += ' ';
        append_number(out, v, precision == 0 ? 7 : precision);
      }
    }
    out += '\n';
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

int sniff_xyz(ByteView data, std::string_view filename) {
  if (has_extension(filename, "csv") || has_extension(filename, "tsv") || !looks_textual(data)) return 0;
  LineReader lines(text_head(data, 4096));
  std::string_view line;
  std::size_t columns = 0, rows = 0;
  bool count_line = false;
  while (lines.next(line) && rows < 20) {
    if (lines.at_end() && data.size() > 4096) break;  // the last line may be cut
    if (comment_line(line)) continue;
    const std::vector<std::string_view> parts = tokens(line, ",;");
    if (parts.size() == 1 && rows == 0 && !count_line) {
      count_line = true;
      continue;
    }
    if (parts.size() < 3 || parts.size() > 10) return 0;
    if (columns == 0) columns = parts.size();
    if (parts.size() != columns) return 0;
    for (const std::string_view p : parts) {
      double v = 0;
      if (!parse_double(p, v)) return 0;
    }
    ++rows;
  }
  if (rows < 3) return 0;
  return has_extension(filename, "xyz") || has_extension(filename, "pts") || has_extension(filename, "xyzn") ||
                 has_extension(filename, "xyzrgb")
             ? 90
             : 66;
}

// --------------------------------------------------------------------------- PCD

struct PcdField {
  std::string name;
  std::size_t size = 4;
  char type = 'F';
  std::size_t count = 1;
  std::size_t offset = 0;  // within a binary record
};

double pcd_value(const std::uint8_t* p, const PcdField& f) {
  util::ByteReader r(ByteView(reinterpret_cast<const std::byte*>(p), f.size));
  if (f.type == 'F') return f.size == 8 ? r.f64le() : r.f32le();
  if (f.type == 'U') {
    switch (f.size) {
      case 1: return r.u8();
      case 2: return r.u16le();
      case 4: return r.u32le();
      default: return static_cast<double>(r.u64le());
    }
  }
  switch (f.size) {
    case 1: return r.i8();
    case 2: return r.i16le();
    case 4: return r.i32le();
    default: return static_cast<double>(r.i64le());
  }
}

Result<AnyIR> read_pcd(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  LineReader lines(text);
  std::string_view line;
  std::vector<PcdField> fields;
  std::uint64_t points = 0, width = 0, height = 1;
  std::string data;
  while (lines.next(line)) {
    std::string_view rest = util::trim(line);
    if (rest.empty() || rest.starts_with('#')) continue;
    const std::string_view key = next_token(rest);
    const std::vector<std::string_view> values = tokens(rest);
    if (key == "VERSION") continue;
    if (key == "FIELDS" || key == "COLUMNS") {
      fields.clear();
      for (const std::string_view v : values) fields.push_back({std::string(v), 4, 'F', 1, 0});
    } else if (key == "SIZE" || key == "TYPE" || key == "COUNT") {
      if (values.size() != fields.size()) return fail(Status::Decode, "PCD {} does not match FIELDS", key);
      for (std::size_t i = 0; i < values.size(); ++i) {
        std::int64_t n = 0;
        if (key == "TYPE") {
          if (values[i].size() != 1 || std::string_view("FUI").find(values[i][0]) == std::string_view::npos) {
            return fail(Status::Decode, "unknown PCD type");
          }
          fields[i].type = values[i][0];
        } else if (!parse_int(values[i], n) || n < 1 || n > 4096) {
          return fail(Status::Decode, "malformed PCD {}", key);
        } else if (key == "SIZE") {
          if (n != 1 && n != 2 && n != 4 && n != 8) return fail(Status::Decode, "unsupported PCD field size {}", n);
          fields[i].size = static_cast<std::size_t>(n);
        } else {
          fields[i].count = static_cast<std::size_t>(n);
        }
      }
    } else if (key == "WIDTH" || key == "HEIGHT" || key == "POINTS") {
      std::int64_t n = 0;
      if (values.empty() || !parse_int(values[0], n) || n < 0) return fail(Status::Decode, "malformed PCD {}", key);
      (key == "WIDTH" ? width : key == "HEIGHT" ? height : points) = static_cast<std::uint64_t>(n);
    } else if (key == "DATA") {
      data = values.empty() ? std::string() : std::string(values[0]);
      break;
    }
  }
  if (fields.empty() || data.empty()) return fail(Status::Decode, "not a PCD file (header incomplete)");
  if (points == 0) points = width * height;
  std::size_t record = 0;
  for (PcdField& f : fields) {
    f.offset = record;
    record += f.size * f.count;
  }
  const auto index_of = [&](std::string_view name) -> int {
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].name == name) return static_cast<int>(i);
    }
    return -1;
  };
  const int ix = index_of("x"), iy = index_of("y"), iz = index_of("z");
  if (ix < 0 || iy < 0 || iz < 0) return fail(Status::Decode, "the PCD file has no x, y, z fields");
  const int inx = index_of("normal_x"), iny = index_of("normal_y"), inz = index_of("normal_z");
  int icolor = index_of("rgb");
  const bool rgba = icolor < 0 && index_of("rgba") >= 0;
  if (rgba) icolor = index_of("rgba");
  const std::size_t body = lines.position();
  const ByteView rest = ctx.input.subspan(std::min(body, ctx.input.size()));

  // Values of every point, one row of all fields' first components.
  CVT_TRY(check_records(ctx, points, data == "ascii" ? 2 * fields.size() : (data == "binary" ? record : 0), rest.size(), "points"));
  std::vector<double> values(static_cast<std::size_t>(points) * fields.size());
  if (data == "ascii") {
    NumberScanner scan(as_chars(rest), false);
    for (std::uint64_t p = 0; p < points; ++p) {
      for (std::size_t f = 0; f < fields.size(); ++f) {
        for (std::size_t c = 0; c < fields[f].count; ++c) {
          double v = 0;
          if (!scan.next(v)) {
            // NaN is written as "nan" by PCL.
            return fail(Status::Decode, "the PCD data is truncated or malformed");
          }
          if (c == 0) values[p * fields.size() + f] = v;
        }
      }
    }
  } else if (data == "binary" || data == "binary_compressed") {
    Bytes unpacked;
    ByteView raw = rest;
    std::vector<std::size_t> column_start;
    if (data == "binary_compressed") {
      util::ByteReader r(rest);
      const std::uint32_t compressed = r.u32le();
      const std::uint32_t uncompressed = r.u32le();
      const ByteView payload = r.bytes(compressed);
      if (!r.ok()) return fail(Status::Decode, "the compressed PCD data is truncated");
      if (uncompressed != points * record) return fail(Status::Decode, "the PCD compressed size does not match the header");
      CVT_TRY(ctx.limits.check_decompressed(uncompressed));
      CVT_TRY_ASSIGN(unpacked, lzf_decompress(payload, uncompressed));
      raw = unpacked;
      std::size_t at = 0;
      for (const PcdField& f : fields) {
        column_start.push_back(at);
        at += static_cast<std::size_t>(points) * f.size * f.count;
      }
    }
    if (raw.size() < points * record) return fail(Status::Decode, "the PCD data is truncated");
    const auto* base = as_u8(raw).data();
    for (std::uint64_t p = 0; p < points; ++p) {
      for (std::size_t f = 0; f < fields.size(); ++f) {
        const std::uint8_t* at = column_start.empty() ? base + p * record + fields[f].offset
                                                      : base + column_start[f] + p * fields[f].size * fields[f].count;
        double v = 0;
        if (static_cast<int>(f) == icolor && fields[f].type == 'F' && fields[f].size == 4) {
          util::ByteReader bits(ByteView(reinterpret_cast<const std::byte*>(at), 4));
          v = bits.u32le();  // packed colour: keep the bits
        } else {
          v = pcd_value(at, fields[f]);
        }
        values[p * fields.size() + f] = v;
      }
    }
  } else {
    return fail(Status::Decode, "unknown PCD data encoding '{}'", data);
  }
  ir::Mesh mesh = empty_cloud();
  std::vector<int> extra;
  for (std::size_t f = 0; f < fields.size(); ++f) {
    const int i = static_cast<int>(f);
    if (i == ix || i == iy || i == iz || i == inx || i == iny || i == inz || i == icolor || fields[f].count != 1) continue;
    if (fields[f].name == "_") continue;  // padding
    extra.push_back(i);
    mesh.attributes.push_back({fields[f].name, {}});
  }
  std::size_t dropped = 0;
  for (std::uint64_t p = 0; p < points; ++p) {
    const double* row = values.data() + p * fields.size();
    if (!std::isfinite(row[ix]) || !std::isfinite(row[iy]) || !std::isfinite(row[iz])) {
      ++dropped;
      continue;
    }
    mesh.positions.push_back({static_cast<float>(row[ix]), static_cast<float>(row[iy]), static_cast<float>(row[iz])});
    if (inx >= 0 && iny >= 0 && inz >= 0) mesh.normals.push_back({static_cast<float>(row[inx]), static_cast<float>(row[iny]), static_cast<float>(row[inz])});
    if (icolor >= 0) {
      // ASCII files print the packed float; recover its bits.
      std::uint32_t packed = 0;
      if (data == "ascii" && fields[icolor].type == 'F') packed = std::bit_cast<std::uint32_t>(static_cast<float>(row[icolor]));
      else packed = static_cast<std::uint32_t>(file_index(row[icolor]) & 0xFFFFFFFFu);
      const double a = rgba ? static_cast<double>(packed >> 24) : -1.0;
      mesh.colors.push_back(color_from_srgb((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF, a));
    }
    for (std::size_t k = 0; k < extra.size(); ++k) mesh.attributes[k].values.push_back(static_cast<float>(row[extra[k]]));
  }
  if (dropped > 0) ctx.warn(std::format("{} points without coordinates (NaN) were dropped.", dropped));
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

Result<Bytes> write_pcd(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, points_traits()));
  const std::string data = ctx.options.get_enum("data", "binary_compressed", {"ascii", "binary", "binary_compressed"});
  const int precision = write_precision(ctx);
  struct Column {
    std::string name;
    char type;
    std::vector<std::uint32_t> bits;  // float bits or packed colour
  };
  std::vector<Column> columns;
  const std::size_t n = mesh.positions.size();
  const auto float_column = [&](std::string name, auto&& get) {
    Column c{std::move(name), 'F', {}};
    c.bits.reserve(n);
    for (std::size_t i = 0; i < n; ++i) c.bits.push_back(std::bit_cast<std::uint32_t>(static_cast<float>(get(i))));
    columns.push_back(std::move(c));
  };
  float_column("x", [&](std::size_t i) { return mesh.positions[i].x; });
  float_column("y", [&](std::size_t i) { return mesh.positions[i].y; });
  float_column("z", [&](std::size_t i) { return mesh.positions[i].z; });
  if (!mesh.colors.empty()) {
    Column c{"rgb", 'F', {}};
    for (const ir::Color& color : mesh.colors) {
      const auto b = color_to_srgb8(color);
      c.bits.push_back((std::uint32_t{b[0]} << 16) | (std::uint32_t{b[1]} << 8) | b[2]);
    }
    columns.push_back(std::move(c));
  }
  if (!mesh.normals.empty()) {
    float_column("normal_x", [&](std::size_t i) { return mesh.normals[i].x; });
    float_column("normal_y", [&](std::size_t i) { return mesh.normals[i].y; });
    float_column("normal_z", [&](std::size_t i) { return mesh.normals[i].z; });
  }
  for (const ir::VertexAttribute& a : mesh.attributes) {
    std::string name;
    for (const char ch : a.name) name.push_back(util::is_ascii_space(ch) ? '_' : ch);
    float_column(name.empty() ? "value" : name, [&](std::size_t i) { return a.values[i]; });
  }
  std::string header = "# .PCD v0.7 - Point Cloud Data file format\nVERSION 0.7\nFIELDS";
  for (const Column& c : columns) header += ' ' + c.name;
  header += "\nSIZE";
  for (std::size_t k = 0; k < columns.size(); ++k) header += " 4";
  header += "\nTYPE";
  for (std::size_t k = 0; k < columns.size(); ++k) header += " F";
  header += "\nCOUNT";
  for (std::size_t k = 0; k < columns.size(); ++k) header += " 1";
  header += std::format("\nWIDTH {}\nHEIGHT 1\nVIEWPOINT 0 0 0 1 0 0 0\nPOINTS {}\nDATA {}\n", n, n, data);
  util::ByteWriter out(header.size() + n * columns.size() * 4 + 16);
  out.text(header);
  if (data == "ascii") {
    std::string line;
    for (std::size_t i = 0; i < n; ++i) {
      line.clear();
      for (std::size_t k = 0; k < columns.size(); ++k) {
        if (k > 0) line += ' ';
        if (columns[k].name == "rgb") {
          // PCL prints the packed colour as the float with the same bits.
          append_number(line, std::bit_cast<float>(columns[k].bits[i]), 0);
        } else {
          append_number(line, std::bit_cast<float>(columns[k].bits[i]), precision);
        }
      }
      line += '\n';
      out.text(line);
    }
  } else if (data == "binary") {
    for (std::size_t i = 0; i < n; ++i) {
      for (const Column& c : columns) out.u32le(c.bits[i]);
    }
  } else {
    util::ByteWriter raw(n * columns.size() * 4);
    for (const Column& c : columns) {
      for (const std::uint32_t bits : c.bits) raw.u32le(bits);
    }
    const Bytes packed = lzf_compress(raw.view());
    out.u32le(static_cast<std::uint32_t>(packed.size())).u32le(static_cast<std::uint32_t>(raw.size())).bytes(packed);
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out.take();
}

int sniff_pcd(ByteView data, std::string_view /*filename*/) {
  const std::string_view head = text_head(data, 1024);
  const bool fields = head.find("\nFIELDS ") != std::string_view::npos || head.starts_with("FIELDS ");
  const bool marker = head.find("\nDATA ") != std::string_view::npos;
  if (fields && marker && (head.find("VERSION") != std::string_view::npos || head.find("POINTS") != std::string_view::npos)) return 94;
  // The PCL writer's comment line, followed by the header (a cut file still
  // names its format).
  if (head.starts_with("# .PCD v") && (fields || head.find("\nVERSION ") != std::string_view::npos)) return 90;
  return 0;
}

// --------------------------------------------------------------------------- LAS

struct LasHeader {
  std::uint8_t major = 1, minor = 2;
  std::uint16_t header_size = 227;
  std::uint32_t offset = 227;
  std::uint32_t vlr_count = 0;
  std::uint8_t format = 0;
  std::uint16_t record_length = 20;
  std::uint64_t points = 0;
  double scale[3] = {0.001, 0.001, 0.001};
  double offset_xyz[3] = {0, 0, 0};
  std::string system, software;
};

std::size_t las_minimum_record(std::uint8_t format) {
  static constexpr std::size_t sizes[] = {20, 28, 26, 34, 57, 63, 30, 36, 38, 59, 67};
  return format <= 10 ? sizes[format] : 0;
}

Result<AnyIR> read_las(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("LASF")) return fail(Status::Decode, "not a LAS file");
  LasHeader h;
  r.skip(2 + 2 + 16);  // file source id, global encoding, GUID
  h.major = r.u8();
  h.minor = r.u8();
  h.system = std::string(util::trim(r.chars(32)));
  h.software = std::string(util::trim(r.chars(32)));
  r.skip(4);  // creation day, year
  h.header_size = r.u16le();
  h.offset = r.u32le();
  h.vlr_count = r.u32le();
  const std::uint8_t raw_format = r.u8();
  h.record_length = r.u16le();
  const std::uint32_t legacy_points = r.u32le();
  r.skip(20);
  for (double& s : h.scale) s = r.f64le();
  for (double& o : h.offset_xyz) o = r.f64le();
  r.skip(48);  // bounds
  if (!r.ok()) return fail(Status::Decode, "the LAS header is truncated");
  if (raw_format & 0x80 || raw_format & 0x40) return fail(Status::Decode, "compressed LAZ point clouds are not supported");
  h.format = raw_format & 0x3F;
  h.points = legacy_points;
  if (h.major != 1 || h.minor > 4) return fail(Status::Decode, "LAS version {}.{} is not supported", h.major, h.minor);
  if (h.minor >= 4 && h.header_size >= 375) {
    r.seek(247);
    const std::uint64_t points64 = r.u64le();
    if (r.ok() && points64 > 0) h.points = points64;
  }
  if (h.format > 10) return fail(Status::Decode, "LAS point format {} is not supported", h.format);
  if (h.record_length < las_minimum_record(h.format)) return fail(Status::Decode, "the LAS point record is too short");
  if (h.offset > ctx.input.size()) return fail(Status::Decode, "the LAS point data offset is outside the file");
  for (const double s : h.scale) {
    if (!(s > 0) || !std::isfinite(s)) return fail(Status::Decode, "invalid LAS scale factors");
  }
  CVT_TRY(check_records(ctx, h.points, h.record_length, ctx.input.size() - h.offset, "points"));

  ir::Mesh mesh = empty_cloud();
  mesh.unit_meters = 1.0;
  if (!h.software.empty()) mesh.metadata["tool"] = h.software;
  if (!h.system.empty()) mesh.metadata["system"] = h.system;
  const bool has_gps = h.format == 1 || h.format >= 3;
  const bool has_rgb = h.format == 2 || h.format == 3 || h.format == 5 || h.format == 7 || h.format == 8 || h.format == 10;
  const bool extended = h.format >= 6;
  std::size_t rgb_at = 0;
  if (has_rgb) rgb_at = h.format == 2 ? 20 : h.format == 3 || h.format == 5 ? 28 : 30;
  ir::VertexAttribute intensity{"intensity", {}}, classification{"classification", {}}, returns{"return_number", {}}, gps{"gps_time", {}};
  mesh.positions.reserve(static_cast<std::size_t>(h.points));
  double largest = 0;
  for (std::uint64_t p = 0; p < h.points; ++p) {
    if ((p & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(0.9 * static_cast<double>(p) / static_cast<double>(h.points)));
    util::ByteReader rec(ctx.input.subspan(h.offset + p * h.record_length, h.record_length));
    const std::int32_t x = rec.i32le(), y = rec.i32le(), z = rec.i32le();
    const double wx = x * h.scale[0] + h.offset_xyz[0], wy = y * h.scale[1] + h.offset_xyz[1], wz = z * h.scale[2] + h.offset_xyz[2];
    largest = std::max({largest, std::abs(wx), std::abs(wy), std::abs(wz)});
    mesh.positions.push_back({static_cast<float>(wx), static_cast<float>(wy), static_cast<float>(wz)});
    intensity.values.push_back(rec.u16le());
    if (extended) {
      const std::uint8_t r1 = rec.u8();
      rec.skip(1);
      returns.values.push_back(r1 & 0x0F);
      classification.values.push_back(rec.u8());
      rec.skip(1 + 2 + 2);
    } else {
      const std::uint8_t r1 = rec.u8();
      returns.values.push_back(r1 & 0x07);
      classification.values.push_back(rec.u8() & 0x1F);
      rec.skip(1 + 1 + 2);
    }
    if (has_gps) gps.values.push_back(static_cast<float>(rec.f64le()));
    if (has_rgb) {
      rec.seek(rgb_at);
      const double red = rec.u16le(), green = rec.u16le(), blue = rec.u16le();
      mesh.colors.push_back(color_from_srgb(red, green, blue, -1.0, 65535.0));
    }
  }
  if (largest > 1e5) {
    ctx.warn("The LAS coordinates are large; single-precision vertices keep about 7 significant digits.");
  }
  mesh.attributes.push_back(std::move(intensity));
  mesh.attributes.push_back(std::move(classification));
  mesh.attributes.push_back(std::move(returns));
  if (has_gps) mesh.attributes.push_back(std::move(gps));
  mesh.source_up_axis = "Z";
  convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

constexpr WriterTraits las_traits() {
  WriterTraits traits = points_traits();
  traits.text = false;
  traits.normals = false;
  traits.up = WriterTraits::Up::Z;
  return traits;
}

Result<Bytes> write_las(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, las_traits()));
  const bool v14 = ctx.options.get_enum("las_version", "1.2", {"1.2", "1.4"}) == "1.4";
  const bool rgb = !mesh.colors.empty();
  const std::uint8_t format = v14 ? (rgb ? 7 : 6) : (rgb ? 3 : 1);
  const std::uint16_t record = static_cast<std::uint16_t>(las_minimum_record(format));
  const std::uint16_t header_size = v14 ? 375 : 227;
  const Bounds box = bounds(mesh);
  // Scale: millimetres when the extent allows, coarser for huge extents.
  double scale = 0.001;
  const double extent = std::max({std::abs(box.min.x), std::abs(box.max.x), std::abs(box.min.y), std::abs(box.max.y),
                                  std::abs(box.min.z), std::abs(box.max.z), static_cast<float>(box.longest_side())});
  while (extent / scale > 2.0e9) scale *= 10;
  // Finer when the data is small (keep about 1e-6 of the extent).
  while (scale > 1e-7 && box.longest_side() / scale < 1e6) scale /= 10;
  const double offset[3] = {std::floor(box.min.x), std::floor(box.min.y), std::floor(box.min.z)};
  const auto find = [&](std::string_view name) -> const ir::VertexAttribute* {
    for (const ir::VertexAttribute& a : mesh.attributes) {
      if (a.name == name) return &a;
    }
    return nullptr;
  };
  const ir::VertexAttribute* intensity = find("intensity");
  const ir::VertexAttribute* classification = find("classification");
  const ir::VertexAttribute* gps = find("gps_time");
  const ir::VertexAttribute* returns = find("return_number");
  const std::size_t n = mesh.positions.size();
  if (!v14 && n > 0xFFFFFFFFull) return fail(Status::Encode, "LAS 1.2 holds at most 4294967295 points; use version 1.4");
  util::ByteWriter out(header_size + n * record);
  out.text("LASF").u16le(0).u16le(0).zeros(16).u8(1).u8(v14 ? 4 : 2);
  std::string software = options::resolved_strip_metadata(ctx.options) ? std::string() : std::string("Convertible");
  software.resize(32, '\0');
  out.zeros(32).text(software);
  out.u16le(0).u16le(0);  // no creation date
  out.u16le(header_size).u32le(header_size).u32le(0).u8(format).u16le(record);
  // Legacy counts: zero for the 1.4 point formats (6-10), which use the 64-bit fields.
  out.u32le(v14 ? 0 : static_cast<std::uint32_t>(n));
  out.u32le(v14 ? 0 : static_cast<std::uint32_t>(n)).zeros(16);  // by return: all first returns
  for (int k = 0; k < 3; ++k) out.f64le(scale);
  for (const double o : offset) out.f64le(o);
  out.f64le(box.max.x).f64le(box.min.x).f64le(box.max.y).f64le(box.min.y).f64le(box.max.z).f64le(box.min.z);
  if (v14) {
    out.u64le(0).u64le(0).u32le(0).u64le(n).u64le(n).zeros(14 * 8);
  }
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(0.8 + 0.2 * static_cast<double>(i) / static_cast<double>(n)));
    const ir::Vec3& p = mesh.positions[i];
    out.i32le(static_cast<std::int32_t>(std::llround((p.x - offset[0]) / scale)));
    out.i32le(static_cast<std::int32_t>(std::llround((p.y - offset[1]) / scale)));
    out.i32le(static_cast<std::int32_t>(std::llround((p.z - offset[2]) / scale)));
    out.u16le(intensity ? static_cast<std::uint16_t>(std::clamp(intensity->values[i], 0.0f, 65535.0f)) : 0);
    const auto return_number = static_cast<std::uint8_t>(returns ? std::clamp(returns->values[i], 1.0f, v14 ? 15.0f : 7.0f) : 1);
    const auto class_value = static_cast<std::uint8_t>(classification ? std::clamp(classification->values[i], 0.0f, v14 ? 255.0f : 31.0f) : 0);
    if (v14) {
      out.u8(static_cast<std::uint8_t>(return_number | (1 << 4))).u8(0).u8(class_value).u8(0).i16le(0).u16le(0);
    } else {
      out.u8(static_cast<std::uint8_t>(return_number | (1 << 3))).u8(class_value).i8(0).u8(0).u16le(0);
    }
    out.f64le(gps ? gps->values[i] : 0.0);
    if (rgb) {
      const auto c = color_to_srgb8(mesh.colors[i]);
      out.u16le(static_cast<std::uint16_t>(c[0] * 257)).u16le(static_cast<std::uint16_t>(c[1] * 257)).u16le(static_cast<std::uint16_t>(c[2] * 257));
    }
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out.take();
}

}  // namespace

void register_point_cloud_files(Registry& registry) {
  FormatInfo xyz;
  xyz.id = "xyz";
  xyz.name = "XYZ point list";
  xyz.description = "Plain text point cloud: one point per line (x y z, optional colours and normals).";
  xyz.category = Category::Model3D;
  xyz.extensions = {"xyz", "pts", "xyzn", "xyzrgb", "txt"};
  xyz.mime = "text/plain";
  xyz.popularity = 20;
  xyz.sniff = sniff_xyz;
  registry.add_format(std::move(xyz));
  Codec xyz_codec;
  xyz_codec.format_id = "xyz";
  xyz_codec.kind = Kind::Mesh;
  xyz_codec.read = read_xyz;
  xyz_codec.write = write_xyz;
  xyz_codec.write_options = mesh_write_options(points_traits());
  registry.add_codec(std::move(xyz_codec));

  FormatInfo pcd;
  pcd.id = "pcd";
  pcd.name = "Point Cloud Data";
  pcd.description = "Point Cloud Library format (ASCII, binary or LZF-compressed).";
  pcd.category = Category::Model3D;
  pcd.extensions = {"pcd"};
  pcd.mime = "application/x-pcd";
  pcd.popularity = 20;
  pcd.sniff = sniff_pcd;
  registry.add_format(std::move(pcd));
  Codec pcd_codec;
  pcd_codec.format_id = "pcd";
  pcd_codec.kind = Kind::Mesh;
  pcd_codec.read = read_pcd;
  pcd_codec.write = write_pcd;
  pcd_codec.write_options = mesh_write_options(points_traits());
  pcd_codec.write_options.insert(pcd_codec.write_options.begin(),
                                 OptionSpec::choice("data", "Encoding", "binary_compressed", {"ascii", "binary", "binary_compressed"},
                                                    "How the points are stored: text, raw binary or LZF-compressed binary."));
  registry.add_codec(std::move(pcd_codec));

  FormatInfo las;
  las.id = "las";
  las.name = "LAS point cloud";
  las.description = "ASPRS LiDAR point cloud (versions 1.0-1.4).";
  las.category = Category::Model3D;
  las.extensions = {"las"};
  las.mime = "application/vnd.las";
  las.popularity = 25;
  las.signatures = {Signature::at(0, "'LASF'")};
  registry.add_format(std::move(las));
  Codec las_codec;
  las_codec.format_id = "las";
  las_codec.kind = Kind::Mesh;
  las_codec.read = read_las;
  las_codec.write = write_las;
  las_codec.write_options = mesh_write_options(las_traits());
  las_codec.write_options.insert(las_codec.write_options.begin(),
                                 OptionSpec::choice("las_version", "LAS version", "1.2", {"1.2", "1.4"},
                                                    "1.2 is read by almost every tool; 1.4 allows more points and classes."));
  registry.add_codec(std::move(las_codec));
}

}  // namespace cvt::mesh
