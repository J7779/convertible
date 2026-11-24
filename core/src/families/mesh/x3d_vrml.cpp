// Convertible core - mesh family: X3D (XML encoding) and VRML 2.0.
//
// Both encodings describe the same scene graph. They are parsed into one
// generic node tree (type, DEF/USE, raw field values, child nodes) and
// interpreted once: Transform/Group-like grouping nodes, Shape with
// Appearance/Material/ImageTexture, IndexedFaceSet, IndexedTriangleSet,
// TriangleSet and the Box/Sphere/Cylinder/Cone primitives. Writers emit one
// Transform per object and one Shape per material, sharing coordinates
// through DEF/USE.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::mesh {

namespace {

struct SgNode {
  std::string type;
  std::string def;
  std::string use;
  std::map<std::string, std::string, std::less<>> fields;
  std::vector<SgNode> children;

  [[nodiscard]] std::string_view field(std::string_view name) const {
    const auto it = fields.find(name);
    return it == fields.end() ? std::string_view() : std::string_view(it->second);
  }
  [[nodiscard]] const SgNode* child(std::string_view child_type) const {
    for (const SgNode& c : children) {
      if (c.type == child_type || (!c.use.empty() && c.type.empty())) {
        if (c.type == child_type) return &c;
      }
    }
    return nullptr;
  }
};

// --------------------------------------------------------------------------- X3D XML -> nodes

Result<void> from_xml(const util::XmlNode& element, SgNode& out, int depth, std::size_t& count) {
  if (depth > 128) return fail(Status::Decode, "the X3D scene is nested too deeply");
  if (++count > 2'000'000) return fail(Status::Limit, "the X3D scene has too many nodes");
  out.type = std::string(element.local_name());
  for (const util::XmlAttribute a : element.attributes()) {
    const std::string_view name = a.local_name();
    if (name == "DEF") out.def = std::string(a.value());
    else if (name == "USE") out.use = std::string(a.value());
    else out.fields.emplace(std::string(name), std::string(a.value()));
  }
  for (const util::XmlNode child : element.elements()) {
    SgNode node;
    CVT_TRY(from_xml(child, node, depth + 1, count));
    out.children.push_back(std::move(node));
  }
  return {};
}

// --------------------------------------------------------------------------- VRML text -> nodes

class VrmlParser {
 public:
  VrmlParser(std::string_view text, const ReadContext& ctx) : text_(text), ctx_(ctx) {}

  Result<SgNode> parse() {
    SgNode root;
    root.type = "Scene";
    for (;;) {
      skip();
      if (pos_ >= text_.size()) break;
      const std::string_view word = peek_word();
      if (word == "ROUTE") {
        word_token();
        word_token();
        word_token();
        word_token();
        continue;
      }
      if (word == "PROTO" || word == "EXTERNPROTO") {
        CVT_TRY(skip_proto());
        continue;
      }
      SgNode node;
      CVT_TRY(parse_node(node, 0));
      root.children.push_back(std::move(node));
    }
    return root;
  }

 private:
  void skip() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '#') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      } else if (util::is_ascii_space(c) || c == ',') {
        ++pos_;
      } else {
        break;
      }
    }
  }
  static bool word_char(char c) {
    return !util::is_ascii_space(c) && c != ',' && c != '{' && c != '}' && c != '[' && c != ']' && c != '"' && c != '#';
  }
  std::string_view word_token() {
    skip();
    const std::size_t start = pos_;
    while (pos_ < text_.size() && word_char(text_[pos_])) ++pos_;
    return text_.substr(start, pos_ - start);
  }
  std::string_view peek_word() {
    const std::size_t save = pos_;
    const std::string_view w = word_token();
    pos_ = save;
    return w;
  }
  char peek_char() {
    skip();
    return pos_ < text_.size() ? text_[pos_] : '\0';
  }
  static bool is_value_start(std::string_view w) {
    if (w.empty()) return false;
    const char c = w.front();
    return util::is_ascii_digit(c) || c == '-' || c == '+' || c == '.' || w == "TRUE" || w == "FALSE";
  }

  Result<void> skip_proto() {
    // PROTO name [ interface ] { body }: skip both bracketed parts.
    word_token();
    word_token();
    for (const char open : {'[', '{'}) {
      if (peek_char() != open) continue;
      const char close = open == '[' ? ']' : '}';
      int level = 0;
      do {
        if (pos_ >= text_.size()) return fail(Status::Decode, "unterminated VRML PROTO");
        if (text_[pos_] == '"') {
          ++pos_;
          while (pos_ < text_.size() && text_[pos_] != '"') ++pos_;
        } else if (text_[pos_] == open) {
          ++level;
        } else if (text_[pos_] == close) {
          --level;
        }
        ++pos_;
      } while (level > 0);
    }
    ctx_.warn("VRML PROTO definitions were skipped.");
    return {};
  }

  Result<void> parse_node(SgNode& node, int depth) {
    if (depth > 128) return fail(Status::Decode, "the VRML scene is nested too deeply");
    if (++count_ > 2'000'000) return fail(Status::Limit, "the VRML scene has too many nodes");
    std::string_view w = word_token();
    if (w == "USE") {
      node.use = std::string(word_token());
      return {};
    }
    if (w == "DEF") {
      node.def = std::string(word_token());
      w = word_token();
    }
    if (w.empty()) return fail(Status::Decode, "malformed VRML node");
    node.type = std::string(w);
    if (peek_char() != '{') return fail(Status::Decode, "VRML node '{}' has no body", node.type);
    ++pos_;
    for (;;) {
      const char c = peek_char();
      if (c == '\0') return fail(Status::Decode, "the VRML file is truncated");
      if (c == '}') {
        ++pos_;
        return {};
      }
      const std::string field(word_token());
      if (field.empty()) return fail(Status::Decode, "malformed VRML field");
      if (field == "ROUTE") {
        word_token();
        word_token();
        word_token();
        continue;
      }
      CVT_TRY(parse_value(node, field, depth));
    }
  }

  Result<void> parse_value(SgNode& node, const std::string& field, int depth) {
    const char c = peek_char();
    if (c == '[') {
      ++pos_;
      std::string values;
      for (;;) {
        const char d = peek_char();
        if (d == '\0') return fail(Status::Decode, "unterminated VRML list");
        if (d == ']') {
          ++pos_;
          break;
        }
        if (d == '"') {
          values += read_string();
          values += ' ';
          continue;
        }
        const std::string_view w = peek_word();
        if (is_value_start(w)) {
          values += word_token();
          values += ' ';
          if ((values.size() & 0xFFFFF) < 32) CVT_TRY(check_elements(ctx_, values.size() / 2, "VRML values"));
        } else if (!w.empty()) {
          SgNode child;
          CVT_TRY(parse_node(child, depth + 1));
          child.fields.emplace("__field", field);
          node.children.push_back(std::move(child));
        } else {
          ++pos_;  // stray character
        }
      }
      if (!values.empty()) node.fields[field] = std::move(values);
      return {};
    }
    if (c == '"') {
      node.fields[field] = read_string();
      return {};
    }
    const std::string_view w = peek_word();
    if (w == "NULL") {
      word_token();
      return {};
    }
    if (is_value_start(w)) {
      std::string values;
      while (is_value_start(peek_word())) {
        values += word_token();
        values += ' ';
      }
      node.fields[field] = std::move(values);
      return {};
    }
    SgNode child;
    CVT_TRY(parse_node(child, depth + 1));
    child.fields.emplace("__field", field);
    node.children.push_back(std::move(child));
    return {};
  }

  std::string read_string() {
    skip();
    ++pos_;  // opening quote
    std::string out;
    while (pos_ < text_.size() && text_[pos_] != '"') {
      if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) ++pos_;
      out += text_[pos_++];
    }
    if (pos_ < text_.size()) ++pos_;
    return out;
  }

  std::string_view text_;
  const ReadContext& ctx_;
  std::size_t pos_ = 0;
  std::size_t count_ = 0;
};

// --------------------------------------------------------------------------- interpretation

std::vector<float> floats(std::string_view text) {
  std::vector<float> out;
  NumberScanner scan(text);
  float v = 0;
  while (scan.next_float(v)) out.push_back(v);
  return out;
}

std::vector<std::int64_t> ints(std::string_view text) {
  std::vector<std::int64_t> out;
  NumberScanner scan(text);
  std::int64_t v = 0;
  while (scan.next_int(v)) out.push_back(v);
  return out;
}

ir::Vec3 vec3(std::string_view text, ir::Vec3 fallback) {
  const std::vector<float> v = floats(text);
  return v.size() >= 3 ? ir::Vec3{v[0], v[1], v[2]} : fallback;
}

bool truthy(std::string_view text, bool fallback) {
  const std::string_view t = util::trim(text);
  if (t.empty()) return fallback;
  return util::iequals(t, "true");
}

ir::Mat4 axis_angle(std::string_view text) {
  const std::vector<float> v = floats(text);
  if (v.size() < 4) return {};
  return rotation(static_cast<float>(v[3] * 180.0 / std::numbers::pi), v[0], v[1], v[2]);
}

class SceneBuilder {
 public:
  SceneBuilder(const ReadContext& ctx) : ctx_(ctx) {}

  Result<ir::Mesh> build(const SgNode& root) {
    collect_defs(root, 0);
    CVT_TRY(visit(root, {}, "", 0));
    return std::move(mesh_);
  }

 private:
  void collect_defs(const SgNode& node, int depth) {
    if (depth > 128) return;
    if (!node.def.empty() && node.use.empty()) defs_.emplace(node.def, &node);
    for (const SgNode& c : node.children) collect_defs(c, depth + 1);
  }

  const SgNode* resolve(const SgNode& node) const {
    if (node.use.empty()) return &node;
    const auto it = defs_.find(node.use);
    return it == defs_.end() ? nullptr : it->second;
  }

  const SgNode* find_child(const SgNode& node, std::initializer_list<std::string_view> types) const {
    for (const SgNode& c : node.children) {
      const SgNode* r = resolve(c);
      if (r == nullptr) continue;
      for (const std::string_view t : types) {
        if (r->type == t) return r;
      }
    }
    return nullptr;
  }

  Result<void> visit(const SgNode& raw, const ir::Mat4& parent, const std::string& name, int depth) {
    if (depth > 64) return fail(Status::Decode, "the scene graph is too deep or its DEF/USE references form a cycle");
    if (++visits_ > 200000) return fail(Status::Limit, "the scene graph has too many node instances");
    const SgNode* node = resolve(raw);
    if (node == nullptr) return {};
    ir::Mat4 world = parent;
    std::string here = name;
    const std::string& type = node->type;
    if (type == "Transform") {
      const ir::Vec3 t = vec3(node->field("translation"), {0, 0, 0});
      const ir::Vec3 c = vec3(node->field("center"), {0, 0, 0});
      const ir::Vec3 s = vec3(node->field("scale"), {1, 1, 1});
      const ir::Mat4 r = axis_angle(node->field("rotation"));
      const ir::Mat4 so = axis_angle(node->field("scaleOrientation"));
      world = parent * translation(t.x, t.y, t.z) * translation(c.x, c.y, c.z) * r * so * scaling(s.x, s.y, s.z) * inverse(so) *
              translation(-c.x, -c.y, -c.z);
      if (!node->def.empty()) here = node->def;
    } else if (type == "Shape") {
      return shape(*node, world, here.empty() ? node->def : here);
    } else if (type == "Inline") {
      ctx_.warn("Inline scenes refer to other files and were skipped.");
      return {};
    } else if (type == "LOD") {
      for (const SgNode& c : node->children) {  // the most detailed level only
        CVT_TRY(visit(c, world, here, depth + 1));
        break;
      }
      return {};
    } else if (!node->def.empty() && here.empty()) {
      here = node->def;
    }
    for (const SgNode& c : node->children) {
      const SgNode* r = resolve(c);
      if (r == nullptr) continue;
      // Only grouping content: skip appearance/geometry-like children of other nodes.
      CVT_TRY(visit(c, world, here, depth + 1));
    }
    return {};
  }

  std::string material(const SgNode* appearance) {
    if (appearance == nullptr) return {};
    if (const auto it = material_names_.find(appearance); it != material_names_.end()) return it->second;
    ir::Material m;
    const SgNode* mat = find_child(*appearance, {"Material"});
    m.name = mat != nullptr && !mat->def.empty() ? mat->def : (!appearance->def.empty() ? appearance->def : std::format("material_{}", mesh_.materials.size()));
    if (mat != nullptr) {
      const ir::Vec3 d = vec3(mat->field("diffuseColor"), {0.8f, 0.8f, 0.8f});
      const std::vector<float> tr = floats(mat->field("transparency"));
      m.base_color = {d.x, d.y, d.z, tr.empty() ? 1.0f : std::clamp(1.0f - tr[0], 0.0f, 1.0f)};
      m.emissive = vec3(mat->field("emissiveColor"), {0, 0, 0});
      const std::vector<float> sh = floats(mat->field("shininess"));
      m.roughness = sh.empty() ? 0.8f : std::clamp(1.0f - sh[0], 0.0f, 1.0f);
    }
    if (const SgNode* tex = find_child(*appearance, {"ImageTexture"})) {
      std::string url(tex->field("url"));
      // X3D writes MFString as '"a.png" "b.png"'; take the first entry.
      if (const std::size_t q = url.find('"'); q != std::string::npos) {
        const std::size_t e = url.find('"', q + 1);
        url = url.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
      }
      url = std::string(util::trim(url));
      if (!url.empty()) {
        m.base_color_texture = static_cast<int>(mesh_.textures.size());
        mesh_.textures.push_back({std::string(base_name(url)), url, image_mime({}, url), {}});
        ctx_.warn(std::format("Texture '{}' is a separate file and was not included.", url));
      }
    }
    for (int k = 2; mesh_.find_material(m.name) != nullptr; ++k) m.name = std::format("{}_{}", m.name, k);
    material_names_[appearance] = m.name;
    mesh_.materials.push_back(m);
    return m.name;
  }

  Result<void> shape(const SgNode& node, const ir::Mat4& world, const std::string& name) {
    const SgNode* appearance = find_child(node, {"Appearance"});
    const SgNode* geometry = find_child(node, {"IndexedFaceSet", "IndexedTriangleSet", "TriangleSet", "Box", "Sphere", "Cylinder", "Cone"});
    if (geometry == nullptr) return {};
    ir::Mesh part;
    CVT_TRY(make_geometry(*geometry, part));
    if (part.positions.empty() || part.indices.empty()) return {};
    part.groups.push_back({name, material(appearance), 0, part.face_count()});
    transform(part, world);
    const std::size_t first = mesh_.face_count();
    const auto materials = std::move(mesh_.materials);
    const auto textures = std::move(mesh_.textures);
    append(mesh_, part);
    mesh_.materials = materials;
    mesh_.textures = textures;
    if (!mesh_.objects.empty() && mesh_.objects.back().name == name && mesh_.objects.back().transform == world &&
        mesh_.objects.back().first_face + mesh_.objects.back().face_count == first) {
      mesh_.objects.back().face_count += mesh_.face_count() - first;
    } else {
      mesh_.objects.push_back({name, first, mesh_.face_count() - first, world});
    }
    return check_elements(ctx_, mesh_.positions.size(), "vertices");
  }

  Result<void> make_geometry(const SgNode& g, ir::Mesh& out) {
    const std::string& type = g.type;
    if (type == "Box") {
      const ir::Vec3 s = vec3(g.field("size"), {2, 2, 2});
      box(out, s);
      return {};
    }
    if (type == "Sphere" || type == "Cylinder" || type == "Cone") {
      revolve(out, g);
      return {};
    }
    std::vector<float> points;
    if (const SgNode* coord = find_child(g, {"Coordinate", "CoordinateDouble"})) points = floats(coord->field("point"));
    CVT_TRY(check_elements(ctx_, points.size(), "coordinates"));
    const std::size_t n = points.size() / 3;
    std::vector<float> normals, uvs, colors;
    std::size_t color_width = 3;
    if (const SgNode* normal = find_child(g, {"Normal"})) normals = floats(normal->field("vector"));
    if (const SgNode* tc = find_child(g, {"TextureCoordinate"})) uvs = floats(tc->field("point"));
    if (const SgNode* color = find_child(g, {"Color", "ColorRGBA"})) {
      colors = floats(color->field("color"));
      color_width = color->type == "ColorRGBA" ? 4 : 3;
    }
    const bool ccw = truthy(g.field("ccw"), true);
    // Faces as lists of corners: (point, normal index, uv index, colour index).
    struct Corner {
      std::int64_t p, n, t, c;
    };
    std::vector<std::vector<Corner>> faces;
    if (type == "IndexedFaceSet") {
      const std::vector<std::int64_t> coord_index = ints(g.field("coordIndex"));
      const std::vector<std::int64_t> normal_index = ints(g.field("normalIndex"));
      const std::vector<std::int64_t> tex_index = ints(g.field("texCoordIndex"));
      const std::vector<std::int64_t> color_index = ints(g.field("colorIndex"));
      const bool normal_per_vertex = truthy(g.field("normalPerVertex"), true);
      const bool color_per_vertex = truthy(g.field("colorPerVertex"), true);
      std::vector<Corner> face;
      std::size_t face_number = 0;
      for (std::size_t i = 0; i <= coord_index.size(); ++i) {
        if (i == coord_index.size() || coord_index[i] < 0) {
          if (face.size() >= 3) faces.push_back(face);
          if (!face.empty()) ++face_number;
          face.clear();
          continue;
        }
        const std::int64_t p = coord_index[i];
        const auto pick = [&](const std::vector<std::int64_t>& index, bool per_vertex) -> std::int64_t {
          if (!per_vertex) return index.empty() ? static_cast<std::int64_t>(face_number) : (face_number < index.size() ? index[face_number] : -1);
          return index.empty() ? p : (i < index.size() ? index[i] : -1);
        };
        face.push_back({p, normals.empty() ? -1 : pick(normal_index, normal_per_vertex), uvs.empty() ? -1 : (tex_index.empty() ? p : (i < tex_index.size() ? tex_index[i] : -1)),
                        colors.empty() ? -1 : pick(color_index, color_per_vertex)});
      }
    } else if (type == "IndexedTriangleSet") {
      const std::vector<std::int64_t> index = ints(g.field("index"));
      for (std::size_t i = 0; i + 2 < index.size(); i += 3) {
        faces.push_back({{index[i], index[i], index[i], index[i]}, {index[i + 1], index[i + 1], index[i + 1], index[i + 1]},
                         {index[i + 2], index[i + 2], index[i + 2], index[i + 2]}});
      }
    } else {  // TriangleSet: consecutive points
      for (std::size_t i = 0; i + 2 < n; i += 3) {
        const auto k = static_cast<std::int64_t>(i);
        faces.push_back({{k, k, k, k}, {k + 1, k + 1, k + 1, k + 1}, {k + 2, k + 2, k + 2, k + 2}});
      }
    }
    struct Key {
      std::int64_t p, n, t, c;
      bool operator<(const Key& o) const { return std::tie(p, n, t, c) < std::tie(o.p, o.n, o.t, o.c); }
    };
    std::map<Key, std::uint32_t> made;
    const auto in_range = [](std::int64_t i, const std::vector<float>& values, std::size_t width) {
      return i >= 0 && static_cast<std::size_t>(i) < values.size() / width;
    };
    for (const auto& face : faces) {
      for (const Corner& corner : face) {
        if (!in_range(corner.p, points, 3)) return fail(Status::Decode, "a face refers to missing coordinate {}", corner.p);
      }
      std::vector<std::uint32_t> corners;
      for (const Corner& corner : face) {
        const Key key{corner.p, in_range(corner.n, normals, 3) ? corner.n : -1, in_range(corner.t, uvs, 2) ? corner.t : -1,
                      in_range(corner.c, colors, color_width) ? corner.c : -1};
        const auto [it, inserted] = made.emplace(key, static_cast<std::uint32_t>(out.positions.size()));
        if (inserted) {
          const auto p = static_cast<std::size_t>(key.p) * 3;
          out.positions.push_back({points[p], points[p + 1], points[p + 2]});
          if (!normals.empty()) {
            const auto q = static_cast<std::size_t>(std::max<std::int64_t>(key.n, 0)) * 3;
            out.normals.push_back(key.n < 0 ? ir::Vec3{0, 0, 0} : ir::Vec3{normals[q], normals[q + 1], normals[q + 2]});
          }
          if (!uvs.empty()) {
            const auto q = static_cast<std::size_t>(std::max<std::int64_t>(key.t, 0)) * 2;
            out.texcoords.push_back(key.t < 0 ? ir::Vec2{} : ir::Vec2{uvs[q], uvs[q + 1]});
          }
          if (!colors.empty()) {
            const auto q = static_cast<std::size_t>(std::max<std::int64_t>(key.c, 0)) * color_width;
            out.colors.push_back(key.c < 0 ? ir::Color{} : ir::Color{colors[q], colors[q + 1], colors[q + 2], color_width == 4 ? colors[q + 3] : 1.0f});
          }
        }
        corners.push_back(it->second);
      }
      if (!ccw) std::reverse(corners.begin() + 1, corners.end());
      out.indices.insert(out.indices.end(), corners.begin(), corners.end());
      out.face_sizes.push_back(static_cast<std::uint32_t>(corners.size()));
      CVT_TRY(check_elements(ctx_, out.indices.size(), "face corners"));
    }
    return {};
  }

  static void box(ir::Mesh& out, const ir::Vec3& size) {
    const float x = size.x / 2, y = size.y / 2, z = size.z / 2;
    out.positions = {{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z}, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}};
    out.indices = {0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 1, 2, 6, 5, 2, 3, 7, 6, 3, 0, 4, 7};
    out.face_sizes.assign(6, 4);
    generate_normals(out, 30.0f);
  }

  // Sphere, cylinder and cone as surfaces of revolution around +Y.
  static void revolve(ir::Mesh& out, const SgNode& g) {
    constexpr int segments = 32;
    std::vector<std::pair<float, float>> profile;  // (radius, height), top to bottom
    bool smooth_caps = false;
    if (g.type == "Sphere") {
      const std::vector<float> r = floats(g.field("radius"));
      const float radius = r.empty() ? 1.0f : r[0];
      for (int k = 0; k <= 16; ++k) {
        const double phi = std::numbers::pi * k / 16;
        profile.emplace_back(static_cast<float>(radius * std::sin(phi)), static_cast<float>(radius * std::cos(phi)));
      }
      smooth_caps = true;
    } else {
      const std::vector<float> h = floats(g.field("height"));
      const float height = h.empty() ? 2.0f : h[0];
      const std::vector<float> r = floats(g.field(g.type == "Cone" ? "bottomRadius" : "radius"));
      const float radius = r.empty() ? 1.0f : r[0];
      if (g.type == "Cone") {
        profile = {{0.0f, height / 2}, {radius, -height / 2}, {0.0f, -height / 2}};
      } else {
        profile = {{0.0f, height / 2}, {radius, height / 2}, {radius, -height / 2}, {0.0f, -height / 2}};
      }
    }
    for (std::size_t ring = 0; ring < profile.size(); ++ring) {
      for (int s = 0; s < segments; ++s) {
        const double theta = 2 * std::numbers::pi * s / segments;
        out.positions.push_back({static_cast<float>(profile[ring].first * std::sin(theta)), profile[ring].second,
                                 static_cast<float>(profile[ring].first * std::cos(theta))});
      }
    }
    for (std::size_t ring = 0; ring + 1 < profile.size(); ++ring) {
      for (int s = 0; s < segments; ++s) {
        const auto a = static_cast<std::uint32_t>(ring * segments + s);
        const auto b = static_cast<std::uint32_t>(ring * segments + (s + 1) % segments);
        const auto c = static_cast<std::uint32_t>((ring + 1) * segments + (s + 1) % segments);
        const auto d = static_cast<std::uint32_t>((ring + 1) * segments + s);
        out.indices.insert(out.indices.end(), {a, d, c, a, c, b});
      }
    }
    weld(out);
    remove_degenerate_faces(out);
    remove_unused_vertices(out);
    generate_normals(out, smooth_caps ? 180.0f : 40.0f);
  }

  const ReadContext& ctx_;
  ir::Mesh mesh_;
  std::map<std::string, const SgNode*, std::less<>> defs_;
  std::map<const SgNode*, std::string> material_names_;
  std::size_t visits_ = 0;
};

Result<AnyIR> finish_scene(Result<ir::Mesh> built, ReadContext& ctx) {
  if (!built) return std::unexpected(std::move(built).error());
  ir::Mesh mesh = std::move(*built);
  if (mesh.positions.empty()) return fail(Status::Decode, "the scene contains no geometry");
  if (mesh.objects.size() == 1 && mesh.objects[0].transform.is_identity()) mesh.objects.clear();
  mesh.source_up_axis = "Y";
  mesh.unit_meters = 1.0;
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

Result<AnyIR> read_x3d(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(ctx.input));
  const util::XmlNode root = doc.root();
  if (!root || root.local_name() != "X3D") return fail(Status::Decode, "not an X3D document");
  const util::XmlNode scene = root.child_local("Scene");
  if (!scene) return fail(Status::Decode, "the X3D document has no Scene");
  SgNode tree;
  std::size_t count = 0;
  CVT_TRY(from_xml(scene, tree, 0, count));
  SceneBuilder builder(ctx);
  auto mesh = builder.build(tree);
  if (mesh) {
    if (const util::XmlNode head = root.child_local("head")) {
      for (const util::XmlNode meta : head.children_local("meta")) {
        const std::string name(meta.attribute("name").value());
        if (name == "generator" || name == "title" || name == "author" || name == "copyright") {
          mesh->metadata[name == "generator" ? "tool" : name] = std::string(meta.attribute("content").value());
        }
      }
    }
  }
  return finish_scene(std::move(mesh), ctx);
}

Result<AnyIR> read_wrl(ReadContext& ctx) {
  Bytes inflated;
  ByteView data = ctx.input;
  if (data.size() >= 2 && std::to_integer<int>(data[0]) == 0x1F && std::to_integer<int>(data[1]) == 0x8B) {
    CVT_TRY_ASSIGN(inflated, util::gzip_decompress(data, ctx.limits.max_decompressed_bytes));
    data = inflated;
  }
  const std::string_view text = as_chars(data);
  if (text.starts_with("#VRML V1.0")) return fail(Status::Decode, "VRML 1.0 files are not supported (only VRML 2.0 / VRML97)");
  if (!text.starts_with("#VRML V2.0")) return fail(Status::Decode, "not a VRML 2.0 file");
  VrmlParser parser(text.substr(text.find('\n') == std::string_view::npos ? text.size() : text.find('\n')), ctx);
  CVT_TRY_ASSIGN(const SgNode tree, parser.parse());
  SceneBuilder builder(ctx);
  return finish_scene(builder.build(tree), ctx);
}

// --------------------------------------------------------------------------- writing

constexpr WriterTraits x3d_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.objects = true;
  traits.up = WriterTraits::Up::Y;
  return traits;
}

std::string number_list(const std::vector<float>& values, int precision, std::size_t group, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += (i % group == 0) ? separator : " ";
    append_number(out, values[i], precision);
  }
  return out;
}

std::string safe_def(std::string_view text, std::string_view fallback, std::map<std::string, int>& used) {
  std::string out;
  for (const char c : text) out.push_back(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-' ? c : '_');
  if (out.empty() || util::is_ascii_digit(out.front()) || out.front() == '-') out = std::string(fallback) + out;
  std::string unique = out;
  for (int k = 2; used.contains(unique); ++k) unique = std::format("{}_{}", out, k);
  used[unique] = 1;
  return unique;
}

Result<Bytes> write_scene(const AnyIR& value, WriteContext& ctx, bool xml) {
  CVT_TRY_ASSIGN(ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, x3d_traits()));
  if (mesh.unit_meters > 0 && mesh.unit_meters != 1.0) convert_units(mesh, 1.0);  // X3D/VRML units are metres
  const int precision = write_precision(ctx);
  std::map<std::string, std::size_t> material_index;
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) material_index.emplace(mesh.materials[i].name, i);
  std::map<std::string, int> used;
  std::string out;
  if (xml) {
    out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<X3D profile=\"Interchange\" version=\"3.3\" "
          "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema-instance\" "
          "xsd:noNamespaceSchemaLocation=\"http://www.web3d.org/specifications/x3d-3.3.xsd\">\n";
    if (!options::resolved_strip_metadata(ctx.options) && !mesh.metadata.empty()) {
      out += "  <head>\n";
      for (const auto& [key, text] : mesh.metadata) {
        out += std::format("    <meta name=\"{}\" content=\"{}\"/>\n", util::xml_escape(key, true), util::xml_escape(text, true));
      }
      out += "  </head>\n";
    }
    out += "  <Scene>\n";
  } else {
    out = "#VRML V2.0 utf8\n\n";
  }
  const std::vector<ObjectPart> parts = split_objects(mesh, false);
  for (std::size_t p = 0; p < parts.size(); ++p) {
    CVT_TRY(ctx.checkpoint(0.8 + 0.2 * static_cast<double>(p) / parts.size()));
    const ir::Mesh& part = parts[p].mesh;
    if (part.indices.empty()) continue;
    const std::string def = safe_def(parts[p].name, "object", used);
    const std::string coords = safe_def(def + "_coords", "coords", used);
    std::vector<float> values;
    for (const ir::Vec3& v : part.positions) values.insert(values.end(), {v.x, v.y, v.z});
    const std::string point_text = number_list(values, precision, 3, xml ? " " : ",\n        ");
    std::string normal_text, uv_text, color_text;
    if (!part.normals.empty()) {
      values.clear();
      for (const ir::Vec3& v : part.normals) values.insert(values.end(), {v.x, v.y, v.z});
      normal_text = number_list(values, precision == 0 ? 7 : precision, 3, xml ? " " : ", ");
    }
    if (!part.texcoords.empty()) {
      values.clear();
      for (const ir::Vec2& v : part.texcoords) values.insert(values.end(), {v.x, v.y});
      uv_text = number_list(values, precision, 2, xml ? " " : ", ");
    }
    const std::size_t color_width = xml ? 4 : 3;
    if (!part.colors.empty()) {
      values.clear();
      for (const ir::Color& c : part.colors) {
        values.insert(values.end(), {c.r, c.g, c.b});
        if (xml) values.push_back(c.a);
      }
      color_text = number_list(values, 6, color_width, xml ? " " : ", ");
    }
    // Shapes per material run.
    const std::size_t faces = part.face_count();
    std::vector<std::string> face_material(faces);
    for (const ir::MeshGroup& g : part.groups) {
      for (std::size_t f = g.first_face; f < g.first_face + g.face_count && f < faces; ++f) face_material[f] = g.material;
    }
    const std::vector<std::size_t> offsets = face_offsets(part);
    out += xml ? std::format("    <Transform DEF=\"{}\">\n", def) : std::format("DEF {} Transform {{\n  children [\n", def);
    bool first_shape = true;
    std::size_t f = 0;
    while (f < faces) {
      const std::string name = face_material[f];
      std::size_t end = f;
      while (end < faces && face_material[end] == name) ++end;
      std::string index;
      for (std::size_t k = f; k < end; ++k) {
        for (std::size_t c = offsets[k]; c < offsets[k + 1]; ++c) index += std::to_string(part.indices[c]) + ' ';
        index += xml ? "-1 " : "-1,\n        ";
      }
      const auto mi = material_index.find(name);
      std::string diffuse = "0.8 0.8 0.8", emissive = "0 0 0", transparency = "0", shininess = "0.2";
      std::string texture;
      if (mi != material_index.end()) {
        const ir::Material& m = mesh.materials[mi->second];
        diffuse = number_list({m.base_color.r, m.base_color.g, m.base_color.b}, 6, 3, " ");
        emissive = number_list({m.emissive.x, m.emissive.y, m.emissive.z}, 6, 3, " ");
        transparency = format_number(1.0 - m.base_color.a, 6);
        shininess = format_number(std::clamp(1.0 - m.roughness, 0.0, 1.0), 6);
        if (m.base_color_texture >= 0) {
          const ir::Texture& t = mesh.textures[static_cast<std::size_t>(m.base_color_texture)];
          texture = std::string(base_name(t.uri.empty() ? t.name : t.uri));
          if (texture.empty()) texture = std::format("texture_{}.png", m.base_color_texture);
        }
      }
      if (xml) {
        out += "      <Shape>\n        <Appearance>\n";
        out += std::format("          <Material diffuseColor=\"{}\" emissiveColor=\"{}\" transparency=\"{}\" shininess=\"{}\"/>\n",
                           diffuse, emissive, transparency, shininess);
        if (!texture.empty()) out += std::format("          <ImageTexture url='\"{}\"'/>\n", util::xml_escape(texture, true));
        out += "        </Appearance>\n";
        out += std::format("        <IndexedFaceSet solid=\"false\" coordIndex=\"{}\"{}>\n", util::trim(index),
                           normal_text.empty() ? "" : " normalPerVertex=\"true\"");
        if (first_shape) {
          out += std::format("          <Coordinate DEF=\"{}\" point=\"{}\"/>\n", coords, point_text);
          if (!normal_text.empty()) out += std::format("          <Normal DEF=\"{}_n\" vector=\"{}\"/>\n", coords, normal_text);
          if (!uv_text.empty()) out += std::format("          <TextureCoordinate DEF=\"{}_t\" point=\"{}\"/>\n", coords, uv_text);
          if (!color_text.empty()) out += std::format("          <ColorRGBA DEF=\"{}_c\" color=\"{}\"/>\n", coords, color_text);
        } else {
          out += std::format("          <Coordinate USE=\"{}\"/>\n", coords);
          if (!normal_text.empty()) out += std::format("          <Normal USE=\"{}_n\"/>\n", coords);
          if (!uv_text.empty()) out += std::format("          <TextureCoordinate USE=\"{}_t\"/>\n", coords);
          if (!color_text.empty()) out += std::format("          <ColorRGBA USE=\"{}_c\"/>\n", coords);
        }
        out += "        </IndexedFaceSet>\n      </Shape>\n";
      } else {
        out += "    Shape {\n      appearance Appearance {\n";
        out += std::format("        material Material {{ diffuseColor {} emissiveColor {} transparency {} shininess {} }}\n", diffuse,
                           emissive, transparency, shininess);
        if (!texture.empty()) out += std::format("        texture ImageTexture {{ url \"{}\" }}\n", texture);
        out += "      }\n      geometry IndexedFaceSet {\n        solid FALSE\n";
        if (first_shape) {
          out += std::format("        coord DEF {} Coordinate {{ point [\n        {} ] }}\n", coords, point_text);
          if (!normal_text.empty()) out += std::format("        normal DEF {}_n Normal {{ vector [ {} ] }}\n", coords, normal_text);
          if (!uv_text.empty()) out += std::format("        texCoord DEF {}_t TextureCoordinate {{ point [ {} ] }}\n", coords, uv_text);
          if (!color_text.empty()) out += std::format("        color DEF {}_c Color {{ color [ {} ] }}\n", coords, color_text);
        } else {
          out += std::format("        coord USE {}\n", coords);
          if (!normal_text.empty()) out += std::format("        normal USE {}_n\n", coords);
          if (!uv_text.empty()) out += std::format("        texCoord USE {}_t\n", coords);
          if (!color_text.empty()) out += std::format("        color USE {}_c\n", coords);
        }
        out += std::format("        coordIndex [\n        {} ]\n      }}\n    }}\n", index);
      }
      first_shape = false;
      f = end;
    }
    out += xml ? "    </Transform>\n" : "  ]\n}\n";
  }
  if (xml) out += "  </Scene>\n</X3D>\n";
  if (!mesh.textures.empty()) ctx.warn("Textures are referenced by file name and were not embedded.");
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

Result<Bytes> write_x3d(const AnyIR& value, WriteContext& ctx) { return write_scene(value, ctx, true); }
Result<Bytes> write_wrl(const AnyIR& value, WriteContext& ctx) { return write_scene(value, ctx, false); }

}  // namespace

void register_x3d_vrml(Registry& registry) {
  FormatInfo x3d;
  x3d.id = "x3d";
  x3d.name = "X3D scene";
  x3d.description = "Web3D X3D scene in XML encoding.";
  x3d.category = Category::Model3D;
  x3d.extensions = {"x3d"};
  x3d.mime = "model/x3d+xml";
  x3d.popularity = 18;
  x3d.sniff = [](ByteView data, std::string_view) -> int {
    const std::string_view head = text_head(data, 2048);
    return head.find("<X3D") != std::string_view::npos ? 92 : 0;
  };
  registry.add_format(std::move(x3d));
  Codec x3d_codec;
  x3d_codec.format_id = "x3d";
  x3d_codec.kind = Kind::Mesh;
  x3d_codec.read = read_x3d;
  x3d_codec.write = write_x3d;
  x3d_codec.write_options = mesh_write_options(x3d_traits());
  registry.add_codec(std::move(x3d_codec));

  FormatInfo wrl;
  wrl.id = "wrl";
  wrl.name = "VRML 2.0 world";
  wrl.description = "VRML97 (VRML 2.0) scene, plain or gzip-compressed.";
  wrl.category = Category::Model3D;
  wrl.extensions = {"wrl", "vrml"};
  wrl.mime = "model/vrml";
  wrl.popularity = 18;
  wrl.signatures = {Signature::at(0, "'#VRML V2.0'")};
  wrl.sniff = [](ByteView data, std::string_view filename) -> int {
    if (as_chars(data).starts_with("#VRML V2.0")) return 96;
    return data.size() > 2 && std::to_integer<int>(data[0]) == 0x1F && (has_extension(filename, "wrl") || has_extension(filename, "wrz")) ? 80 : 0;
  };
  registry.add_format(std::move(wrl));
  Codec wrl_codec;
  wrl_codec.format_id = "wrl";
  wrl_codec.kind = Kind::Mesh;
  wrl_codec.read = read_wrl;
  wrl_codec.write = write_wrl;
  wrl_codec.write_options = mesh_write_options(x3d_traits());
  registry.add_codec(std::move(wrl_codec));
}

}  // namespace cvt::mesh
