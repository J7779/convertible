// Convertible core - vector family: registration of DXF (vector + Mesh codecs) and DWG detection.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "engine/registry.hpp"
#include "families/mesh/common.hpp"
#include "families/vector/cad_dxf.hpp"
#include "families/vector/cad_internal.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/text.hpp"

namespace cvt::vector::cad {

namespace {

int sniff_dxf(std::span<const std::byte> data, std::string_view /*filename*/) {
  if (is_binary_dxf(data)) return 100;
  std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 4096)));
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  // Read the first few group code / value pairs, skipping 999 comments.
  std::size_t pos = 0;
  const auto line = [&]() -> std::optional<std::string_view> {
    if (pos >= text.size()) return std::nullopt;
    const std::size_t start = pos;
    while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') ++pos;
    const std::string_view l = text.substr(start, pos - start);
    if (pos < text.size() && text[pos] == '\r') ++pos;
    if (pos < text.size() && text[pos] == '\n') ++pos;
    return l;
  };
  for (int pair = 0; pair < 8; ++pair) {
    auto code = line();
    while (code && util::trim(*code).empty() && pair == 0) code = line();
    const auto value = line();
    if (!code || !value) return 0;
    const std::string_view c = util::trim(*code);
    const std::string_view v = util::trim(*value);
    if (c == "999") continue;
    if (c != "0" || v != "SECTION") return 0;
    const auto code2 = line();
    const auto value2 = line();
    if (!code2 || !value2) return 70;
    const std::string_view name = util::trim(*value2);
    if (util::trim(*code2) == "2" &&
        (name == "HEADER" || name == "CLASSES" || name == "TABLES" || name == "BLOCKS" || name == "ENTITIES" ||
         name == "OBJECTS")) {
      return 96;
    }
    return 60;
  }
  return 0;
}

// --------------------------------------------------------------------------- DWG

struct DwgVersion {
  std::string_view magic;
  std::string_view release;
};
constexpr std::array<DwgVersion, 16> dwg_versions = {{
    {"AC1.40", "AutoCAD 1.40"},          {"AC1.50", "AutoCAD 2.05"},         {"AC2.10", "AutoCAD 2.10"},
    {"AC1001", "AutoCAD 2.22"},          {"AC1002", "AutoCAD 2.50"},         {"AC1003", "AutoCAD 2.60"},
    {"AC1004", "AutoCAD Release 9"},     {"AC1006", "AutoCAD Release 10"},   {"AC1009", "AutoCAD Release 11/12"},
    {"AC1012", "AutoCAD Release 13"},    {"AC1014", "AutoCAD Release 14"},   {"AC1015", "AutoCAD 2000-2002"},
    {"AC1018", "AutoCAD 2004-2006"},     {"AC1021", "AutoCAD 2007-2009"},    {"AC1024", "AutoCAD 2010-2012"},
    {"AC1027", "AutoCAD 2013-2017"},
}};

std::string dwg_release(std::string_view magic) {
  if (magic == "AC1032") return "AutoCAD 2018-2024";
  for (const DwgVersion& v : dwg_versions) {
    if (v.magic == magic) return std::string(v.release);
  }
  return "unknown AutoCAD release";
}

bool dwg_magic(std::span<const std::byte> data) {
  if (data.size() < 6) return false;
  const std::string_view m = as_chars(data.first(6));
  if (!m.starts_with("AC")) return false;
  if (m == "AC1.40" || m == "AC1.50" || m == "AC2.10") return true;
  return m.starts_with("AC10") && util::is_ascii_digit(m[4]) && util::is_ascii_digit(m[5]);
}

int sniff_dwg(std::span<const std::byte> data, std::string_view /*filename*/) { return dwg_magic(data) ? 95 : 0; }

Result<Scene> read_dwg(ReadContext& ctx) {
  if (!dwg_magic(ctx.input)) return fail(Status::Decode, "this is not a DWG file");
  const std::string magic(as_chars(ctx.input.first(6)));
  return fail(Status::Decode, "DWG ({}, {}) is not supported yet; export DXF from your CAD program", dwg_release(magic),
              magic);
}

Result<AnyIR> read_mesh(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Scene scene, read_dxf_for_mesh(ctx));
  if (!scene.mesh || scene.mesh->positions.empty()) {
    return fail(Status::Decode, "the DXF has no 3D faces (3DFACE, polyface or polygon mesh, MESH)");
  }
  ir::Mesh mesh = std::move(*scene.mesh);
  mesh.source_up_axis = "Z";  // DXF is +Z up; the reader has turned it into +Y up
  CVT_TRY(mesh::finish_read(mesh, ctx));
  return AnyIR(std::move(mesh));
}

// 3D faces only: no normals, texture coordinates, colours or materials; +Z
// up, so the shared preparation turns the mesh's +Y up into it (and the
// reader turns it back).
mesh::WriterTraits dxf_mesh_traits() {
  mesh::WriterTraits traits;
  traits.up = mesh::WriterTraits::Up::Z;
  traits.normals = false;
  traits.texcoords = false;
  traits.colors = false;
  traits.materials = false;
  return traits;
}

Result<Bytes> write_mesh(const AnyIR& value, WriteContext& ctx) {
  // The mesh family's shared options and preparation (quality, metadata,
  // optional arrays, axis), as for every other mesh writer.
  CVT_TRY_ASSIGN(const ir::Mesh prepared, mesh::prepare_for_write(value.get<ir::Mesh>(), ctx, dxf_mesh_traits()));
  return write_mesh_dxf(prepared, ctx);
}

}  // namespace

void register_dxf(Registry& registry) {
  VectorFormat format;
  format.info.id = "dxf";
  format.info.name = "AutoCAD DXF drawing";
  format.info.description = "Drawing Exchange Format of CAD programs (ASCII or binary, R12 to 2018).";
  format.info.category = Category::Vector;
  format.info.extensions = {"dxf"};
  format.info.mime = "image/vnd.dxf";
  format.info.popularity = 55;
  format.info.signatures = {Signature::at(0, "'AutoCAD Binary DXF' 0D 0A 1A 00")};
  format.info.sniff = sniff_dxf;
  format.read = read_dxf;
  format.write = write_dxf;
  format.lossy_read = true;
  format.lossy_write = true;
  format.can_embed_images = false;
  format.read_options = {
      OptionSpec::choice("space", "Drawing space", "model", {"model", "paper"},
                         "Draw model space (the drawing) or paper space (the layout sheet)."),
      OptionSpec::text("dxf_layers", "Layers", "",
                       "Comma-separated layer names to draw; empty draws every layer."),
      OptionSpec::number("max_page_size", "Largest page size (px)", 4096.0, 0.0, 10000000.0,
                         "When rendering, drawings larger than this are scaled down to fit; 0 keeps the true scale.")
          .advanced(),
  };
  format.write_options = {
      OptionSpec::choice("version", "DXF version", "2000", {"r12", "2000", "2018"},
                         "r12 (AC1009) for old programs, 2000 (AC1015) or 2018 (AC1032, UTF-8 text)."),
      OptionSpec::choice("units", "Drawing units", "auto", {"auto", "mm", "cm", "m", "in", "ft", "px"},
                         "Unit of the DXF coordinates; auto uses the source's unit, else millimetres."),
      OptionSpec::boolean("fills", "Fills", true, "Write filled areas as solid HATCH entities (2000 and 2018)."),
      OptionSpec::integer("precision", "Decimals", 6, 1, 12, "Digits after the decimal point.").advanced(),
  };
  register_vector_format(registry, std::move(format));

  Codec mesh_codec;
  mesh_codec.format_id = "dxf";
  mesh_codec.kind = Kind::Mesh;
  mesh_codec.read = read_mesh;
  mesh_codec.write = write_mesh;
  mesh_codec.lossy = true;
  mesh_codec.read_cost = 20;
  mesh_codec.write_cost = 20;
  mesh_codec.read_options = {};
  mesh_codec.write_options = mesh::mesh_write_options(dxf_mesh_traits());
  registry.add_codec(std::move(mesh_codec));
}

void register_dwg(Registry& registry) {
  VectorFormat format;
  format.info.id = "dwg";
  format.info.name = "AutoCAD DWG drawing";
  format.info.description = "Native binary drawing of AutoCAD (detected; not readable yet).";
  format.info.category = Category::Vector;
  format.info.extensions = {"dwg"};
  format.info.mime = "image/vnd.dwg";
  format.info.popularity = 45;
  format.info.sniff = sniff_dwg;
  format.read = read_dwg;
  format.lossy_read = true;
  format.document_reader = false;
  format.read_cost = 50;
  register_vector_format(registry, std::move(format));
}

}  // namespace cvt::vector::cad
