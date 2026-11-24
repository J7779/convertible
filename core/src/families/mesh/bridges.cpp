// Convertible core - mesh family: bridges from Mesh to Image, Data and Table.
//
//   Mesh -> Image  shaded preview picture (or turntable animation), so any
//                  model converts to PNG, JPEG, GIF, WebP, ...
//   Mesh -> Data   a JSON summary: counts, bounds, area, volume, materials
//   Mesh -> Table  the vertex list (and optionally the faces) as sheets
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "families/mesh/render.hpp"
#include "ir/data.hpp"
#include "ir/image.hpp"
#include "ir/table.hpp"

namespace cvt::mesh {

namespace {

Json vec_json(const ir::Vec3& v) { return Json::array({static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)}); }

double rounded(double value) {
  if (!std::isfinite(value)) return 0.0;
  double out = 0;
  return parse_double(format_number(value, 9), out) ? out : value;
}

Json summary(const ir::Mesh& mesh, bool strip) {
  Json root = Json::object();
  if (!mesh.name.empty()) root["name"] = mesh.name;
  root["point_cloud"] = mesh.point_cloud;
  root["vertices"] = static_cast<std::uint64_t>(mesh.positions.size());
  root["faces"] = static_cast<std::uint64_t>(mesh.point_cloud ? 0 : mesh.face_count());
  root["triangles"] = static_cast<std::uint64_t>(mesh.point_cloud ? 0 : triangle_count(mesh));
  if (!mesh.face_sizes.empty()) {
    std::map<std::uint32_t, std::uint64_t> histogram;
    for (const std::uint32_t size : mesh.face_sizes) ++histogram[size];
    Json sizes = Json::object();
    for (const auto& [size, count] : histogram) sizes[std::to_string(size)] = count;
    root["face_sizes"] = std::move(sizes);
  }
  const Bounds box = bounds(mesh);
  root["bounds"] = Json::object({{"min", vec_json(box.min)}, {"max", vec_json(box.max)}, {"size", vec_json(box.size())},
                                 {"center", vec_json(box.center())}});
  if (!mesh.point_cloud) {
    root["surface_area"] = rounded(surface_area(mesh));
    ir::Mesh copy = mesh;
    triangulate(copy);
    const std::size_t flips = fix_winding(copy);
    (void)flips;
    root["volume"] = rounded(std::abs(signed_volume(copy)));
  }
  root["has_normals"] = !mesh.normals.empty();
  root["has_texcoords"] = !mesh.texcoords.empty();
  root["has_colors"] = !mesh.colors.empty();
  if (!mesh.attributes.empty()) {
    Json names = Json::array();
    for (const ir::VertexAttribute& a : mesh.attributes) names.push_back(a.name);
    root["attributes"] = std::move(names);
  }
  if (mesh.unit_meters > 0) root["unit_meters"] = mesh.unit_meters;
  if (!mesh.source_up_axis.empty()) root["source_up_axis"] = mesh.source_up_axis;
  Json materials = Json::array();
  for (const ir::Material& m : mesh.materials) {
    Json material = Json::object({{"name", m.name},
                                  {"base_color", Json::array({rounded(m.base_color.r), rounded(m.base_color.g),
                                                              rounded(m.base_color.b), rounded(m.base_color.a)})},
                                  {"metallic", rounded(m.metallic)},
                                  {"roughness", rounded(m.roughness)}});
    if (m.base_color_texture >= 0 && static_cast<std::size_t>(m.base_color_texture) < mesh.textures.size()) {
      const ir::Texture& t = mesh.textures[m.base_color_texture];
      material["texture"] = Json::object({{"name", t.name}, {"mime", t.mime},
                                          {"bytes", static_cast<std::uint64_t>(t.data.size())}});
    }
    materials.push_back(std::move(material));
  }
  if (!materials.empty()) root["materials"] = std::move(materials);
  Json objects = Json::array();
  for (const ir::MeshObject& o : mesh.objects) {
    objects.push_back(Json::object({{"name", o.name}, {"faces", static_cast<std::uint64_t>(o.face_count)},
                                    {"translation", Json::array({rounded(o.transform.m[12]), rounded(o.transform.m[13]),
                                                                 rounded(o.transform.m[14])})}}));
  }
  if (!objects.empty()) root["objects"] = std::move(objects);
  Json groups = Json::array();
  for (const ir::MeshGroup& g : mesh.groups) {
    groups.push_back(Json::object({{"name", g.name}, {"material", g.material}, {"faces", static_cast<std::uint64_t>(g.face_count)}}));
  }
  if (!groups.empty()) root["groups"] = std::move(groups);
  if (!strip && !mesh.metadata.empty()) {
    Json meta = Json::object();
    for (const auto& [key, value] : mesh.metadata) meta[key] = value;
    root["metadata"] = std::move(meta);
  }
  return root;
}

ir::Cell number_cell(double value) { return ir::Cell::numeric(value, format_number(value)); }

ir::Table vertex_table(const ir::Mesh& mesh, bool faces) {
  ir::Table table;
  ir::Sheet sheet;
  sheet.name = "vertices";
  sheet.has_header = true;
  std::vector<ir::Cell> header;
  for (const char* name : {"x", "y", "z"}) header.push_back(ir::Cell::string(name));
  if (!mesh.normals.empty()) {
    for (const char* name : {"nx", "ny", "nz"}) header.push_back(ir::Cell::string(name));
  }
  if (!mesh.texcoords.empty()) {
    for (const char* name : {"u", "v"}) header.push_back(ir::Cell::string(name));
  }
  if (!mesh.colors.empty()) {
    for (const char* name : {"red", "green", "blue", "alpha"}) header.push_back(ir::Cell::string(name));
  }
  for (const ir::VertexAttribute& a : mesh.attributes) header.push_back(ir::Cell::string(a.name));
  sheet.rows.push_back(std::move(header));
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    std::vector<ir::Cell> row;
    const ir::Vec3& p = mesh.positions[i];
    for (const float v : {p.x, p.y, p.z}) row.push_back(number_cell(v));
    if (!mesh.normals.empty()) {
      for (const float v : {mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z}) row.push_back(number_cell(v));
    }
    if (!mesh.texcoords.empty()) {
      for (const float v : {mesh.texcoords[i].x, mesh.texcoords[i].y}) row.push_back(number_cell(v));
    }
    if (!mesh.colors.empty()) {
      const auto c = color_to_srgb8(mesh.colors[i]);
      for (const std::uint8_t v : c) row.push_back(number_cell(v));
    }
    for (const ir::VertexAttribute& a : mesh.attributes) row.push_back(number_cell(a.values[i]));
    sheet.rows.push_back(std::move(row));
  }
  table.sheets.push_back(std::move(sheet));
  if (faces && !mesh.point_cloud) {
    ir::Sheet face_sheet;
    face_sheet.name = "faces";
    face_sheet.has_header = true;
    const std::vector<std::size_t> offsets = face_offsets(mesh);
    std::size_t widest = 3;
    for (std::size_t f = 0; f + 1 < offsets.size(); ++f) widest = std::max(widest, offsets[f + 1] - offsets[f]);
    std::vector<ir::Cell> head;
    for (std::size_t k = 0; k < widest; ++k) head.push_back(ir::Cell::string(std::format("v{}", k + 1)));
    face_sheet.rows.push_back(std::move(head));
    for (std::size_t f = 0; f + 1 < offsets.size(); ++f) {
      std::vector<ir::Cell> row;
      for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) row.push_back(number_cell(mesh.indices[c]));
      face_sheet.rows.push_back(std::move(row));
    }
    table.sheets.push_back(std::move(face_sheet));
  }
  return table;
}

}  // namespace

void register_mesh_bridges(Registry& registry) {
  Bridge image;
  image.from = Kind::Mesh;
  image.to = Kind::Image;
  image.cost = 25;
  image.lossy = true;
  image.label = "render a preview of the model";
  image.options = {
      OptionSpec::integer("render_size", "Picture size", 512, 16, 4096, "Width and height of the rendered picture in pixels."),
      OptionSpec::choice("render_view", "View", "iso", {"iso", "front", "back", "left", "right", "top", "bottom"},
                         "Direction the model is seen from."),
      OptionSpec::text("render_background", "Background", "transparent",
                       "transparent, light, dark, or a colour as six hexadecimal digits (RRGGBB)."),
      OptionSpec::integer("turntable_frames", "Turntable frames", 1, 1, 120,
                          "Number of frames of a turntable animation; 1 renders a still picture."),
      OptionSpec::number("render_yaw", "Turn (degrees)", 0.0, -360.0, 360.0, "Extra rotation around the vertical axis.").advanced(),
      OptionSpec::number("render_pitch", "Tilt (degrees)", 0.0, -90.0, 90.0, "Extra tilt of the camera.").advanced(),
      OptionSpec::integer("render_supersample", "Anti-aliasing", 2, 1, 4, "Samples per pixel along each axis.").advanced(),
  };
  image.fn = [](AnyIR value, Context& ctx) -> Result<AnyIR> {
    const ir::Mesh mesh = std::move(value).get<ir::Mesh>();
    RenderOptions options;
    options.size = static_cast<std::uint32_t>(ctx.options.get_int("render_size", 512, 16, 4096));
    options.view = ctx.options.get_enum("render_view", "iso", {"iso", "front", "back", "left", "right", "top", "bottom"});
    options.background = ctx.options.get_string("render_background", "transparent");
    options.frames = static_cast<std::uint32_t>(ctx.options.get_int("turntable_frames", 1, 1, 120));
    options.yaw = static_cast<float>(ctx.options.get_float("render_yaw", 0.0, -360.0, 360.0));
    options.pitch = static_cast<float>(ctx.options.get_float("render_pitch", 0.0, -90.0, 90.0));
    options.supersample = static_cast<std::uint32_t>(ctx.options.get_int("render_supersample", 2, 1, 4));
    CVT_TRY_ASSIGN(ir::Image picture, render_mesh(mesh, options, ctx));
    return picture;
  };
  registry.add_bridge(std::move(image));

  Bridge data;
  data.from = Kind::Mesh;
  data.to = Kind::Data;
  data.cost = 20;
  data.lossy = true;
  data.label = "summarise the model";
  data.options = {options::strip_metadata()};
  data.fn = [](AnyIR value, Context& ctx) -> Result<AnyIR> {
    const ir::Mesh& mesh = value.get<ir::Mesh>();
    ir::Data out;
    out.value = summary(mesh, options::resolved_strip_metadata(ctx.options));
    return out;
  };
  registry.add_bridge(std::move(data));

  Bridge table;
  table.from = Kind::Mesh;
  table.to = Kind::Table;
  table.cost = 25;
  table.lossy = true;
  table.label = "list the vertices of the model";
  table.options = {OptionSpec::boolean("include_faces", "Include faces", false, "Add a second sheet listing the faces.")};
  table.fn = [](AnyIR value, Context& ctx) -> Result<AnyIR> {
    const ir::Mesh& mesh = value.get<ir::Mesh>();
    return vertex_table(mesh, ctx.options.get_bool("include_faces", false));
  };
  registry.add_bridge(std::move(table));
}

}  // namespace cvt::mesh
