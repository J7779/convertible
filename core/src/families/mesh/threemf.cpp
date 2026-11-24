// Convertible core - mesh family: 3D Manufacturing Format (3MF) and AMF.
//
// 3MF: an OPC (ZIP) package whose root relationship points at the model
// part (usually 3D/3dmodel.model). Objects hold meshes or components
// (references to other objects with transforms); build items place
// objects. Base materials and the materials extension's colour groups
// carry colours. Units are declared on the model; the axes are Z-up.
//
// AMF: the older XML format (optionally zipped) with objects, volumes,
// materials, vertex colours and constellations of instances.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"
#include "util/zip.hpp"

namespace cvt::mesh {

namespace {

double unit_to_meters(std::string_view unit) {
  if (unit == "micron") return 1e-6;
  if (unit == "centimeter") return 0.01;
  if (unit == "inch") return 0.0254;
  if (unit == "foot") return 0.3048;
  if (unit == "meter") return 1.0;
  return 0.001;  // millimeter, the default
}

std::string_view meters_to_unit(double meters) {
  if (std::abs(meters - 1e-6) < 1e-12) return "micron";
  if (std::abs(meters - 0.01) < 1e-9) return "centimeter";
  if (std::abs(meters - 0.0254) < 1e-9) return "inch";
  if (std::abs(meters - 0.3048) < 1e-9) return "foot";
  if (std::abs(meters - 1.0) < 1e-9) return "meter";
  return "millimeter";
}

// "#RRGGBB" or "#RRGGBBAA" (sRGB) -> linear colour.
std::optional<ir::Color> parse_hex_color(std::string_view text) {
  text = util::trim(text);
  if (!text.starts_with('#') || (text.size() != 7 && text.size() != 9)) return std::nullopt;
  const auto byte = [&](std::size_t at) { return util::parse_int(text.substr(at, 2), 16); };
  const auto r = byte(1), g = byte(3), b = byte(5);
  if (!r || !g || !b) return std::nullopt;
  double a = -1.0;
  if (text.size() == 9) {
    const auto alpha = byte(7);
    if (!alpha) return std::nullopt;
    a = static_cast<double>(*alpha);
  }
  return color_from_srgb(static_cast<double>(*r), static_cast<double>(*g), static_cast<double>(*b), a);
}

std::string hex_color(const ir::Color& c) {
  const auto b = color_to_srgb8(c);
  if (b[3] == 255) return std::format("#{:02X}{:02X}{:02X}", b[0], b[1], b[2]);
  return std::format("#{:02X}{:02X}{:02X}{:02X}", b[0], b[1], b[2], b[3]);
}

// 3MF transforms: twelve numbers m00 m01 m02 m10 ... m32 for row vectors.
ir::Mat4 parse_3mf_transform(std::string_view text, bool& ok) {
  NumberScanner scan(text);
  float v[12];
  ok = true;
  for (float& x : v) {
    if (!scan.next_float(x)) {
      ok = false;
      return {};
    }
  }
  ir::Mat4 m;
  for (int c = 0; c < 3; ++c) {
    for (int r = 0; r < 3; ++r) m.m[c * 4 + r] = v[c * 3 + r];
  }
  for (int r = 0; r < 3; ++r) m.m[12 + r] = v[9 + r];
  return m;
}

std::string format_3mf_transform(const ir::Mat4& m) {
  std::string out;
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 3; ++r) {
      if (!out.empty()) out += ' ';
      append_number(out, m.m[c * 4 + r]);
    }
  }
  return out;
}

struct ObjectDef {
  std::string name;
  util::XmlNode node;
};

struct MaterialRef {
  enum class Kind { None, Base, Colors } kind = Kind::None;
  std::vector<std::string> names;   // base materials
  std::vector<ir::Color> colors;    // base display colours or colour group entries
};

class ThreeMfReader {
 public:
  explicit ThreeMfReader(ReadContext& ctx) : ctx_(ctx) {}

  Result<ir::Mesh> read(const util::XmlDocument& doc) {
    const util::XmlNode model = doc.root();
    if (!model || model.local_name() != "model") return fail(Status::Decode, "the 3MF model part has no <model> root");
    mesh_.unit_meters = unit_to_meters(model.attribute("unit").value());
    for (const util::XmlNode& child : model.elements()) {
      if (child.local_name() == "metadata") {
        const std::string name(child.attribute("name").value());
        const std::string value = child.text();
        if (!name.empty() && !value.empty()) mesh_.metadata[name] = value;
      }
    }
    const util::XmlNode resources = model.child_local("resources");
    if (!resources) return fail(Status::Decode, "the 3MF model has no resources");
    for (const util::XmlNode& r : resources.elements()) {
      const std::string id(r.attribute("id").value());
      if (r.local_name() == "object") {
        objects_[id] = {std::string(r.attribute("name").value()), r};
      } else if (r.local_name() == "basematerials") {
        MaterialRef ref;
        ref.kind = MaterialRef::Kind::Base;
        for (const util::XmlNode& base : r.children_local("base")) {
          ref.names.emplace_back(base.attribute("name").value());
          ref.colors.push_back(parse_hex_color(base.attribute("displaycolor").value()).value_or(ir::Color{}));
          if (ref.names.back().empty()) ref.names.back() = std::format("material_{}_{}", id, ref.names.size() - 1);
          register_material(ref.names.back(), ref.colors.back());
        }
        materials_[id] = std::move(ref);
      } else if (r.local_name() == "colorgroup") {
        MaterialRef ref;
        ref.kind = MaterialRef::Kind::Colors;
        for (const util::XmlNode& color : r.children_local("color")) {
          ref.colors.push_back(parse_hex_color(color.attribute("color").value()).value_or(ir::Color{}));
        }
        materials_[id] = std::move(ref);
      }
    }
    const util::XmlNode build = model.child_local("build");
    if (!build) return fail(Status::Decode, "the 3MF model has no build");
    std::size_t item_index = 0;
    for (const util::XmlNode& item : build.children_local("item")) {
      CVT_TRY(ctx_.checkpoint(0.2 + 0.7 * static_cast<double>(item_index++) / (objects_.size() + 1.0)));
      ir::Mat4 transform;
      if (const auto t = item.attribute("transform")) {
        bool ok = false;
        transform = parse_3mf_transform(t.value(), ok);
        if (!ok) return fail(Status::Decode, "malformed 3MF build transform");
      }
      const std::string id(item.attribute("objectid").value());
      const std::size_t first = mesh_.face_count();
      CVT_TRY(instantiate(id, transform, 0));
      const auto it = objects_.find(id);
      const std::string name = it != objects_.end() && !it->second.name.empty() ? it->second.name : std::format("object_{}", id);
      if (mesh_.face_count() > first) mesh_.objects.push_back({name, first, mesh_.face_count() - first, transform});
    }
    return std::move(mesh_);
  }

 private:
  Result<void> instantiate(const std::string& id, const ir::Mat4& transform, int depth) {
    if (depth > 32) return fail(Status::Decode, "3MF components are nested too deeply or cyclic");
    if (++instances_ > 100000) return fail(Status::Limit, "the 3MF build has too many object instances");
    const auto it = objects_.find(id);
    if (it == objects_.end()) return fail(Status::Decode, "3MF object {} does not exist", id);
    const util::XmlNode node = it->second.node;
    if (const util::XmlNode m = node.child_local("mesh")) {
      CVT_TRY_ASSIGN(ir::Mesh part, read_mesh(node, m));
      transform_part(part, transform);
      append(mesh_, part);
      CVT_TRY(check_elements(ctx_, mesh_.positions.size(), "vertices"));
      return {};
    }
    if (const util::XmlNode components = node.child_local("components")) {
      for (const util::XmlNode& c : components.children_local("component")) {
        if (c.attribute_local("path")) {
          ctx_.warn("3MF components stored in other model parts (production extension) were skipped.");
          continue;
        }
        ir::Mat4 local;
        if (const auto t = c.attribute("transform")) {
          bool ok = false;
          local = parse_3mf_transform(t.value(), ok);
          if (!ok) return fail(Status::Decode, "malformed 3MF component transform");
        }
        CVT_TRY(instantiate(std::string(c.attribute("objectid").value()), transform * local, depth + 1));
      }
    }
    return {};
  }

  static void transform_part(ir::Mesh& part, const ir::Mat4& transform) { mesh::transform(part, transform); }

  Result<ir::Mesh> read_mesh(const util::XmlNode& object, const util::XmlNode& m) {
    ir::Mesh part;
    const util::XmlNode vertices = m.child_local("vertices");
    const util::XmlNode triangles = m.child_local("triangles");
    if (!vertices || !triangles) return fail(Status::Decode, "a 3MF mesh lacks vertices or triangles");
    for (const util::XmlNode& v : vertices.children_local("vertex")) {
      part.positions.push_back({static_cast<float>(v.attribute("x").as_double()), static_cast<float>(v.attribute("y").as_double()),
                                static_cast<float>(v.attribute("z").as_double())});
      if ((part.positions.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx_, part.positions.size(), "vertices"));
    }
    const std::string object_pid(object.attribute("pid").value());
    const long long object_pindex = object.attribute("pindex").as_int64(0);
    // Corners carry colours when a colour group is referenced: emit corners unwelded.
    std::vector<ir::Color> corner_colors;
    std::vector<std::string> face_material;
    bool any_color = false;
    for (const util::XmlNode& t : triangles.children_local("triangle")) {
      const long long v1 = t.attribute("v1").as_int64(-1), v2 = t.attribute("v2").as_int64(-1), v3 = t.attribute("v3").as_int64(-1);
      const auto n = static_cast<long long>(part.positions.size());
      if (v1 < 0 || v2 < 0 || v3 < 0 || v1 >= n || v2 >= n || v3 >= n) {
        return fail(Status::Decode, "a 3MF triangle refers to a missing vertex");
      }
      part.indices.insert(part.indices.end(), {static_cast<std::uint32_t>(v1), static_cast<std::uint32_t>(v2),
                                               static_cast<std::uint32_t>(v3)});
      const std::string pid = t.attribute("pid") ? std::string(t.attribute("pid").value()) : object_pid;
      const long long p1 = t.attribute("p1") ? t.attribute("p1").as_int64(0) : object_pindex;
      const long long p2 = t.attribute("p2") ? t.attribute("p2").as_int64(p1) : p1;
      const long long p3 = t.attribute("p3") ? t.attribute("p3").as_int64(p1) : p1;
      std::string material;
      ir::Color c[3] = {{}, {}, {}};
      if (const auto it = materials_.find(pid); it != materials_.end()) {
        const MaterialRef& ref = it->second;
        const auto at = [&](long long i) { return i >= 0 && static_cast<std::size_t>(i) < ref.colors.size() ? ref.colors[i] : ir::Color{}; };
        if (ref.kind == MaterialRef::Kind::Base) {
          if (p1 >= 0 && static_cast<std::size_t>(p1) < ref.names.size()) {
            material = ref.names[p1];
            register_material(material, ref.colors[p1]);
          }
        } else {
          c[0] = at(p1);
          c[1] = at(p2);
          c[2] = at(p3);
          any_color = true;
        }
      }
      corner_colors.insert(corner_colors.end(), {c[0], c[1], c[2]});
      face_material.push_back(std::move(material));
    }
    if (any_color) {
      ir::Mesh split;
      for (std::size_t i = 0; i < part.indices.size(); ++i) {
        split.positions.push_back(part.positions[part.indices[i]]);
        split.colors.push_back(corner_colors[i]);
        split.indices.push_back(static_cast<std::uint32_t>(i));
      }
      weld(split, 0.0f, false, 0.0f);
      part.positions = std::move(split.positions);
      part.colors = std::move(split.colors);
      part.indices = std::move(split.indices);
    }
    for (std::size_t f = 0; f < face_material.size(); ++f) {
      if (part.groups.empty() || part.groups.back().material != face_material[f]) {
        part.groups.push_back({std::string(object.attribute("name").value()), face_material[f], f, 0});
      }
      part.groups.back().face_count++;
    }
    if (part.groups.size() == 1 && part.groups[0].material.empty()) part.groups.clear();
    part.materials = materials_out_;
    return part;
  }

  void register_material(const std::string& name, const ir::Color& color) {
    for (const ir::Material& m : materials_out_) {
      if (m.name == name) return;
    }
    ir::Material material;
    material.name = name;
    material.base_color = color;
    materials_out_.push_back(material);
  }

  ReadContext& ctx_;
  ir::Mesh mesh_;
  std::map<std::string, ObjectDef> objects_;
  std::map<std::string, MaterialRef> materials_;
  std::vector<ir::Material> materials_out_;
  std::size_t instances_ = 0;
};


// Root relationship target of an OPC package ("/3D/3dmodel.model").
std::string model_part(util::ZipReader& zip) {
  if (auto rels = zip.read("_rels/.rels")) {
    if (auto doc = util::parse_xml(*rels)) {
      for (const util::XmlNode& rel : doc->root().children_local("Relationship")) {
        const std::string_view type = rel.attribute("Type").value();
        if (type.ends_with("/3dmodel")) {
          std::string target(rel.attribute("Target").value());
          while (target.starts_with('/')) target.erase(0, 1);
          return target;
        }
      }
    }
  }
  return "3D/3dmodel.model";
}

Result<AnyIR> read_3mf(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  const std::string part = model_part(zip);
  std::optional<std::size_t> index = zip.find(part);
  if (!index) {
    for (std::size_t i = 0; i < zip.entries().size() && !index; ++i) {
      if (util::iends_with(zip.entries()[i].name, ".model")) index = i;
    }
  }
  if (!index) return fail(Status::Decode, "the 3MF package has no 3D model part");
  CVT_TRY_ASSIGN(const Bytes xml, zip.read(*index));
  CVT_TRY(ctx.checkpoint(0.1, "Parsing"));
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(xml));
  ThreeMfReader reader(ctx);
  CVT_TRY_ASSIGN(ir::Mesh mesh, reader.read(doc));
  if (mesh.positions.empty()) return fail(Status::Decode, "the 3MF build contains no geometry");
  mesh.source_up_axis = "Z";
  convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

constexpr WriterTraits threemf_traits() {
  WriterTraits traits;
  traits.triangles_only = true;
  traits.normals = false;
  traits.texcoords = false;
  traits.objects = true;
  traits.up = WriterTraits::Up::Z;
  return traits;
}

std::string vertex_xml(const ir::Vec3& p, std::string_view open, std::string_view x_tag, std::string_view y_tag,
                       std::string_view z_tag, std::string_view close) {
  std::string out(open);
  out += x_tag;
  append_number(out, p.x);
  out += y_tag;
  append_number(out, p.y);
  out += z_tag;
  append_number(out, p.z);
  out += close;
  return out;
}

Result<Bytes> write_3mf(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, threemf_traits()));
  const UnitChoice unit = resolve_unit(mesh, ctx, 0.001);
  if (unit.scale != 1.0f) {
    scale(mesh, unit.scale);
    for (ir::MeshObject& object : mesh.objects) {
      for (int r = 0; r < 3; ++r) object.transform.m[12 + r] *= unit.scale;
    }
  }
  const bool strip = options::resolved_strip_metadata(ctx.options);
  const bool colors = !mesh.colors.empty();
  std::string xml;
  xml.reserve(mesh.positions.size() * 64 + mesh.indices.size() * 24 + 1024);
  xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  xml += std::format("<model unit=\"{}\" xml:lang=\"en-US\" "
                     "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" "
                     "xmlns:m=\"http://schemas.microsoft.com/3dmanufacturing/material/2015/02\">\n",
                     meters_to_unit(unit.meters));
  if (!strip) {
    for (const auto& [key, text] : mesh.metadata) {
      xml += std::format("  <metadata name=\"{}\">{}</metadata>\n", util::xml_escape(key, true), util::xml_escape(text));
    }
  }
  xml += "  <resources>\n";
  std::map<std::string, std::size_t> material_index;
  int next_id = 1;
  int base_id = 0;
  if (!mesh.materials.empty()) {
    base_id = next_id++;
    xml += std::format("    <basematerials id=\"{}\">\n", base_id);
    for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
      const ir::Material& m = mesh.materials[i];
      xml += std::format("      <base name=\"{}\" displaycolor=\"{}\"/>\n", util::xml_escape(m.name, true), hex_color(m.base_color));
      material_index.emplace(m.name, i);
    }
    xml += "    </basematerials>\n";
  }
  const std::vector<ObjectPart> parts = split_objects(mesh, true);
  std::vector<std::pair<int, ir::Mat4>> build;
  for (std::size_t p = 0; p < parts.size(); ++p) {
    CVT_TRY(ctx.checkpoint(0.8 + 0.15 * static_cast<double>(p) / parts.size()));
    const ir::Mesh& part = parts[p].mesh;
    if (part.indices.empty()) continue;
    int color_id = 0;
    if (colors) {
      color_id = next_id++;
      xml += std::format("    <m:colorgroup id=\"{}\">\n", color_id);
      for (const ir::Color& c : part.colors) xml += std::format("      <m:color color=\"{}\"/>\n", hex_color(c));
      xml += "    </m:colorgroup>\n";
    }
    const int object_id = next_id++;
    xml += std::format("    <object id=\"{}\" type=\"model\"", object_id);
    if (!parts[p].name.empty()) xml += std::format(" name=\"{}\"", util::xml_escape(parts[p].name, true));
    xml += ">\n      <mesh>\n        <vertices>\n";
    for (const ir::Vec3& v : part.positions) xml += vertex_xml(v, "          <vertex", " x=\"", "\" y=\"", "\" z=\"", "\"/>\n");
    xml += "        </vertices>\n        <triangles>\n";
    std::vector<std::string> face_material(part.face_count());
    for (const ir::MeshGroup& g : part.groups) {
      for (std::size_t f = g.first_face; f < g.first_face + g.face_count && f < face_material.size(); ++f) face_material[f] = g.material;
    }
    for (std::size_t f = 0; f < part.face_count(); ++f) {
      const std::uint32_t a = part.indices[f * 3], b = part.indices[f * 3 + 1], c = part.indices[f * 3 + 2];
      xml += std::format("          <triangle v1=\"{}\" v2=\"{}\" v3=\"{}\"", a, b, c);
      if (colors) {
        xml += std::format(" pid=\"{}\" p1=\"{}\" p2=\"{}\" p3=\"{}\"", color_id, a, b, c);
      } else if (const auto it = material_index.find(face_material[f]); it != material_index.end()) {
        xml += std::format(" pid=\"{}\" p1=\"{}\"", base_id, it->second);
      }
      xml += "/>\n";
    }
    xml += "        </triangles>\n      </mesh>\n    </object>\n";
    build.emplace_back(object_id, parts[p].transform);
  }
  if (build.empty()) return fail(Status::Encode, "the model has no triangles");
  xml += "  </resources>\n  <build>\n";
  for (const auto& [id, transform] : build) {
    xml += std::format("    <item objectid=\"{}\"", id);
    if (!transform.is_identity()) xml += std::format(" transform=\"{}\"", format_3mf_transform(transform));
    xml += "/>\n";
  }
  xml += "  </build>\n</model>\n";

  util::ZipWriter zip;
  CVT_TRY(zip.add("[Content_Types].xml",
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                  "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                  "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>"
                  "</Types>\n"));
  CVT_TRY(zip.add("_rels/.rels",
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                  "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
                  "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>"
                  "</Relationships>\n"));
  util::ZipEntryOptions entry;
  entry.level = options::resolved_compression_level(ctx.options);
  CVT_TRY(zip.add("3D/3dmodel.model", xml, entry));
  CVT_TRY_ASSIGN(Bytes out, zip.finish());
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

int sniff_3mf(ByteView data, std::string_view /*filename*/) {
  if (data.size() < 30 || !as_chars(data).starts_with("PK")) return 0;
  Limits limits;
  limits.max_archive_entries = 10000;
  auto zip = util::ZipReader::open(data, limits);
  if (!zip) return 0;
  for (const util::ZipEntry& entry : zip->entries()) {
    if (util::iends_with(entry.name, ".model") && util::istarts_with(entry.name, "3D/")) return 95;
  }
  return 0;
}

// --------------------------------------------------------------------------- AMF

double child_number(const util::XmlNode& node, std::string_view name, double fallback) {
  const util::XmlNode child = node.child_local(name);
  if (!child) return fallback;
  double value = fallback;
  return parse_double(util::trim(child.text()), value) ? value : fallback;
}

std::optional<ir::Color> amf_color(const util::XmlNode& node) {
  const util::XmlNode c = node.child_local("color");
  if (!c) return std::nullopt;
  return ir::Color{static_cast<float>(child_number(c, "r", 1)), static_cast<float>(child_number(c, "g", 1)),
                   static_cast<float>(child_number(c, "b", 1)), static_cast<float>(child_number(c, "a", 1))};
}

Result<AnyIR> read_amf(ReadContext& ctx) {
  Bytes unzipped;
  ByteView xml = ctx.input;
  if (as_chars(ctx.input).starts_with("PK")) {
    CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
    std::optional<std::size_t> entry;
    for (std::size_t i = 0; i < zip.entries().size() && !entry; ++i) {
      if (!zip.entries()[i].is_directory) entry = i;
    }
    if (!entry) return fail(Status::Decode, "the zipped AMF file is empty");
    CVT_TRY_ASSIGN(unzipped, zip.read(*entry));
    xml = unzipped;
  }
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(xml));
  const util::XmlNode root = doc.root();
  if (!root || root.local_name() != "amf") return fail(Status::Decode, "not an AMF document");
  ir::Mesh mesh;
  mesh.unit_meters = unit_to_meters(root.attribute("unit").value());
  std::map<std::string, std::string> material_names;
  for (const util::XmlNode& node : root.elements()) {
    if (node.local_name() == "metadata") {
      const std::string type(node.attribute("type").value());
      if (!type.empty()) mesh.metadata[type] = node.text();
    } else if (node.local_name() == "material") {
      ir::Material material;
      const std::string id(node.attribute("id").value());
      material.name = std::format("material_{}", id);
      for (const util::XmlNode& meta : node.children_local("metadata")) {
        if (meta.attribute("type").value() == "name" && !util::trim(meta.text()).empty()) {
          material.name = std::string(util::trim(meta.text()));
        }
      }
      if (const auto c = amf_color(node)) material.base_color = *c;
      material_names[id] = material.name;
      mesh.materials.push_back(std::move(material));
    }
  }
  std::map<std::string, ir::Mesh> objects;
  std::map<std::string, std::string> object_names;
  for (const util::XmlNode& object : root.children_local("object")) {
    const std::string id(object.attribute("id").value());
    ir::Mesh part;
    part.materials = mesh.materials;
    std::string name = std::format("object_{}", id);
    for (const util::XmlNode& meta : object.children_local("metadata")) {
      if (meta.attribute("type").value() == "name") name = std::string(util::trim(meta.text()));
    }
    object_names[id] = name;
    const util::XmlNode m = object.child_local("mesh");
    if (!m) continue;
    bool any_color = false;
    for (const util::XmlNode& v : m.child_local("vertices").children_local("vertex")) {
      const util::XmlNode c = v.child_local("coordinates");
      part.positions.push_back({static_cast<float>(child_number(c, "x", 0)), static_cast<float>(child_number(c, "y", 0)),
                                static_cast<float>(child_number(c, "z", 0))});
      const auto color = amf_color(v);
      any_color = any_color || color.has_value();
      part.colors.push_back(color.value_or(ir::Color{}));
      if ((part.positions.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx, part.positions.size(), "vertices"));
    }
    if (!any_color) part.colors.clear();
    for (const util::XmlNode& volume : m.children_local("volume")) {
      const std::string material_id(volume.attribute("materialid").value());
      const std::size_t first = part.face_count();
      for (const util::XmlNode& t : volume.children_local("triangle")) {
        const double v1 = child_number(t, "v1", -1), v2 = child_number(t, "v2", -1), v3 = child_number(t, "v3", -1);
        const auto n = static_cast<double>(part.positions.size());
        if (!(v1 >= 0 && v2 >= 0 && v3 >= 0 && v1 < n && v2 < n && v3 < n)) {
          return fail(Status::Decode, "an AMF triangle refers to a missing vertex");
        }
        part.indices.insert(part.indices.end(), {static_cast<std::uint32_t>(v1), static_cast<std::uint32_t>(v2),
                                                 static_cast<std::uint32_t>(v3)});
      }
      const auto it = material_names.find(material_id);
      part.groups.push_back({name, it == material_names.end() ? "" : it->second, first, part.face_count() - first});
    }
    objects[id] = std::move(part);
  }
  // Constellations place objects; without one every object appears once.
  bool placed = false;
  for (const util::XmlNode& constellation : root.children_local("constellation")) {
    for (const util::XmlNode& instance : constellation.children_local("instance")) {
      const std::string id(instance.attribute("objectid").value());
      const auto it = objects.find(id);
      if (it == objects.end()) continue;
      const ir::Mat4 t = translation(static_cast<float>(child_number(instance, "deltax", 0)),
                                     static_cast<float>(child_number(instance, "deltay", 0)),
                                     static_cast<float>(child_number(instance, "deltaz", 0))) *
                         rotation(static_cast<float>(child_number(instance, "rz", 0)), 0, 0, 1) *
                         rotation(static_cast<float>(child_number(instance, "ry", 0)), 0, 1, 0) *
                         rotation(static_cast<float>(child_number(instance, "rx", 0)), 1, 0, 0);
      ir::Mesh part = it->second;
      transform(part, t);
      const std::size_t first = mesh.face_count();
      append(mesh, part);
      mesh.objects.push_back({object_names[id], first, mesh.face_count() - first, t});
      placed = true;
      CVT_TRY(check_elements(ctx, mesh.positions.size(), "vertices"));
    }
  }
  if (!placed) {
    for (auto& [id, part] : objects) {
      const std::size_t first = mesh.face_count();
      append(mesh, part);
      mesh.objects.push_back({object_names[id], first, mesh.face_count() - first, {}});
    }
  }
  if (mesh.objects.size() == 1 && mesh.objects[0].transform.is_identity()) mesh.objects.clear();
  if (mesh.positions.empty()) return fail(Status::Decode, "the AMF file contains no geometry");
  mesh.source_up_axis = "Z";
  convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

std::string amf_color_xml(const ir::Color& c) {
  std::string out = "<color><r>";
  append_number(out, c.r, 6);
  out += "</r><g>";
  append_number(out, c.g, 6);
  out += "</g><b>";
  append_number(out, c.b, 6);
  out += "</b><a>";
  append_number(out, c.a, 6);
  out += "</a></color>";
  return out;
}

Result<Bytes> write_amf(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, threemf_traits()));
  const UnitChoice unit = resolve_unit(mesh, ctx, 0.001);
  if (unit.scale != 1.0f) scale(mesh, unit.scale);
  mesh.objects.clear();  // written in scene coordinates as one object
  std::string xml = std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<amf unit=\"{}\" version=\"1.1\">\n",
                                meters_to_unit(unit.meters));
  if (!options::resolved_strip_metadata(ctx.options)) {
    for (const auto& [key, text] : mesh.metadata) {
      xml += std::format("  <metadata type=\"{}\">{}</metadata>\n", util::xml_escape(key, true), util::xml_escape(text));
    }
  }
  // Every volume gets a material: some readers drop volumes without one.
  std::vector<std::string> face_material(mesh.face_count());
  for (const ir::MeshGroup& group : mesh.groups) {
    for (std::size_t f = group.first_face; f < group.first_face + group.face_count && f < face_material.size(); ++f) {
      face_material[f] = group.material;
    }
  }
  if (std::any_of(face_material.begin(), face_material.end(), [&](const std::string& name) { return mesh.find_material(name) == nullptr; })) {
    ir::Material fallback;
    fallback.name = "default";
    for (int k = 2; mesh.find_material(fallback.name) != nullptr; ++k) fallback.name = std::format("default_{}", k);
    fallback.base_color = {0.8f, 0.8f, 0.8f, 1.0f};
    for (std::string& name : face_material) {
      if (mesh.find_material(name) == nullptr) name = fallback.name;
    }
    mesh.materials.push_back(fallback);
  }
  std::map<std::string, std::size_t> material_id;
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
    const ir::Material& m = mesh.materials[i];
    material_id.emplace(m.name, i + 1);
    xml += std::format("  <material id=\"{}\">\n    <metadata type=\"name\">{}</metadata>\n    {}\n  </material>\n", i + 1,
                       util::xml_escape(m.name), amf_color_xml(m.base_color));
  }
  xml += "  <object id=\"0\">\n";
  if (!mesh.name.empty()) xml += std::format("    <metadata type=\"name\">{}</metadata>\n", util::xml_escape(mesh.name));
  xml += "    <mesh>\n      <vertices>\n";
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    xml += vertex_xml(mesh.positions[i], "        <vertex><coordinates>", "<x>", "</x><y>", "</y><z>", "</z></coordinates>");
    if (!mesh.colors.empty()) xml += amf_color_xml(mesh.colors[i]);
    xml += "</vertex>\n";
  }
  xml += "      </vertices>\n";
  std::size_t f = 0;
  while (f < face_material.size()) {
    const std::string material = face_material[f];
    xml += "      <volume";
    if (const auto it = material_id.find(material); it != material_id.end()) xml += std::format(" materialid=\"{}\"", it->second);
    xml += ">\n";
    for (; f < face_material.size() && face_material[f] == material; ++f) {
      xml += std::format("        <triangle><v1>{}</v1><v2>{}</v2><v3>{}</v3></triangle>\n", mesh.indices[f * 3],
                         mesh.indices[f * 3 + 1], mesh.indices[f * 3 + 2]);
    }
    xml += "      </volume>\n";
  }
  xml += "    </mesh>\n  </object>\n</amf>\n";
  if (!ctx.options.get_bool("compress", false)) {
    CVT_TRY(ctx.limits.check_output(xml.size()));
    return to_bytes(xml);
  }
  util::ZipWriter zip;
  CVT_TRY(zip.add("model.amf", xml));
  CVT_TRY_ASSIGN(Bytes out, zip.finish());
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

int sniff_amf(ByteView data, std::string_view filename) {
  if (as_chars(data).starts_with("PK")) {
    if (!has_extension(filename, "amf")) return 0;
    Limits limits;
    limits.max_archive_entries = 16;
    auto zip = util::ZipReader::open(data, limits);
    return zip && zip->size() >= 1 && zip->size() <= 2 ? 85 : 0;
  }
  const std::string_view head = text_head(data, 1024);
  const std::size_t at = head.find("<amf");
  if (at == std::string_view::npos) return 0;
  const std::string_view before = head.substr(0, at);
  return before.find('<') == std::string_view::npos || before.find("<?xml") != std::string_view::npos ? 92 : 50;
}

}  // namespace

void register_3mf_and_amf(Registry& registry) {
  FormatInfo info;
  info.id = "3mf";
  info.name = "3D Manufacturing Format";
  info.description = "Modern 3D-printing package with units, colours, materials and multiple objects.";
  info.category = Category::Model3D;
  info.extensions = {"3mf"};
  info.mime = "model/3mf";
  info.popularity = 50;
  info.sniff = sniff_3mf;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "3mf";
  codec.kind = Kind::Mesh;
  codec.read = read_3mf;
  codec.write = write_3mf;
  codec.write_options = mesh_write_options(threemf_traits());
  codec.write_options.push_back(unit_option());
  codec.write_options.push_back(options::compression_level());
  registry.add_codec(std::move(codec));

  FormatInfo amf;
  amf.id = "amf";
  amf.name = "Additive Manufacturing File";
  amf.description = "XML 3D-printing format (ISO/ASTM 52915), plain or zipped.";
  amf.category = Category::Model3D;
  amf.extensions = {"amf"};
  amf.mime = "application/x-amf";
  amf.popularity = 12;
  amf.sniff = sniff_amf;
  registry.add_format(std::move(amf));

  Codec amf_codec;
  amf_codec.format_id = "amf";
  amf_codec.kind = Kind::Mesh;
  amf_codec.read = read_amf;
  amf_codec.write = write_amf;
  amf_codec.write_options = mesh_write_options(threemf_traits());
  std::erase_if(amf_codec.write_options, [](const OptionSpec& s) { return s.key == "merge_objects"; });
  amf_codec.write_options.push_back(unit_option());
  amf_codec.write_options.push_back(
      OptionSpec::boolean("compress", "Zip the file", false, "Store the AMF document inside a ZIP archive (smaller)."));
  registry.add_codec(std::move(amf_codec));
}

}  // namespace cvt::mesh
