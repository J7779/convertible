// Convertible core - mesh family: Universal Scene Description (usda, usdz).
//
// usda: the text form of a USD layer. The parser understands the grammar
// that scene files use (layer metadata, def/over/class prims, typed
// attributes with metadata, relationships, time samples, dictionaries) and
// interprets Xform/Mesh/GeomSubset/Material/Shader prims: transform ops in
// xformOpOrder, points/faces/normals, primvars for texture coordinates and
// colours (any interpolation, indexed or not), UsdPreviewSurface materials
// and UsdUVTexture file inputs, upAxis and metersPerUnit.
//
// usdz: an uncompressed ZIP whose first file is the root layer, with every
// file's data 64-byte aligned. Packages with a usda root layer are read and
// written; binary "crate" layers (usdc) are recognised and rejected with a
// clear message.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/crc.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::mesh {

namespace {

// ------------------------------------------------------------------------- tokens and values

struct UsdValue {
  std::vector<double> numbers;  // every number, flattened (tuples and lists)
  std::string text;             // string, token, asset path or prim path
  bool is_path = false;
  std::vector<std::string> texts;  // every string/token of a list
};

struct UsdAttribute {
  std::string type;
  UsdValue value;
  std::map<std::string, std::string, std::less<>> meta;  // interpolation, ...
  std::string connection;  // target of name.connect
};

struct UsdPrim {
  std::string specifier;
  std::string type;
  std::string name;
  std::string path;
  std::map<std::string, UsdAttribute, std::less<>> attributes;
  std::map<std::string, std::string, std::less<>> relationships;
  std::vector<std::unique_ptr<UsdPrim>> children;
};

class UsdaParser {
 public:
  UsdaParser(std::string_view text, const Context& ctx) : text_(text), ctx_(ctx) {}

  Result<std::unique_ptr<UsdPrim>> parse(std::map<std::string, std::string, std::less<>>& layer_meta) {
    if (!text_.starts_with("#usda ")) return fail(Status::Decode, "not a USD text layer (#usda header missing)");
    pos_ = text_.find('\n');
    if (pos_ == std::string_view::npos) pos_ = text_.size();
    auto root = std::make_unique<UsdPrim>();
    root->path = "";
    skip();
    if (peek() == '(') CVT_TRY(metadata(layer_meta));
    for (;;) {
      skip();
      if (pos_ >= text_.size()) break;
      if (peek() == '}') return fail(Status::Decode, "unbalanced '}' in the USD file");
      CVT_TRY(statement(*root, 0));
    }
    return root;
  }

 private:
  void skip() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '#') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      } else if (util::is_ascii_space(c)) {
        ++pos_;
      } else {
        break;
      }
    }
  }
  char peek() {
    skip();
    return pos_ < text_.size() ? text_[pos_] : '\0';
  }
  bool eat(char c) {
    if (peek() != c) return false;
    ++pos_;
    return true;
  }
  static bool name_char(char c) {
    return util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == ':' || c == '.' || c == '[' || c == ']' || c == '-' || c == '+';
  }
  std::string_view word() {
    skip();
    const std::size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (!name_char(c)) break;
      // "[]" belongs to a type name only.
      if (c == '[' && !(pos_ + 1 < text_.size() && text_[pos_ + 1] == ']')) break;
      if (c == ']' && !(pos_ > start && text_[pos_ - 1] == '[')) break;
      ++pos_;
    }
    return text_.substr(start, pos_ - start);
  }
  Result<std::string> string_literal() {
    skip();
    const char quote = text_[pos_];
    const bool triple = text_.substr(pos_, 3) == std::string(3, quote);
    pos_ += triple ? 3 : 1;
    std::string out;
    while (pos_ < text_.size()) {
      if (triple && text_.substr(pos_, 3) == std::string(3, quote)) {
        pos_ += 3;
        return out;
      }
      const char c = text_[pos_++];
      if (!triple && c == quote) return out;
      if (c == '\\' && pos_ < text_.size()) {
        const char e = text_[pos_++];
        out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        continue;
      }
      out += c;
    }
    return fail(Status::Decode, "unterminated string in the USD file");
  }

  // Any value; numbers are flattened.
  Result<void> value(UsdValue& out, int depth) {
    if (depth > 32) return fail(Status::Decode, "USD values are nested too deeply");
    const char c = peek();
    if (c == '"' || c == '\'') {
      CVT_TRY_ASSIGN(out.text, string_literal());
      return {};
    }
    if (c == '@') {
      const bool triple = text_.substr(pos_, 3) == "@@@";
      pos_ += triple ? 3 : 1;
      const std::size_t end = text_.find(triple ? "@@@" : "@", pos_);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated asset path in the USD file");
      out.text = std::string(text_.substr(pos_, end - pos_));
      pos_ = end + (triple ? 3 : 1);
      return {};
    }
    if (c == '<') {
      const std::size_t end = text_.find('>', pos_);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated path in the USD file");
      out.text = std::string(text_.substr(pos_ + 1, end - pos_ - 1));
      out.is_path = true;
      pos_ = end + 1;
      return {};
    }
    if (c == '[' || c == '(') {
      const char close = c == '[' ? ']' : ')';
      ++pos_;
      for (;;) {
        const char d = peek();
        if (d == close) {
          ++pos_;
          break;
        }
        if (d == '\0') return fail(Status::Decode, "unterminated list in the USD file");
        if (d == ',') {
          ++pos_;
          continue;
        }
        UsdValue item;
        CVT_TRY(value(item, depth + 1));
        if (!item.text.empty()) {
          if (out.text.empty()) {
            out.text = item.text;
            out.is_path = item.is_path;
          }
          out.texts.push_back(item.text);
        }
        out.numbers.insert(out.numbers.end(), item.numbers.begin(), item.numbers.end());
        if ((out.numbers.size() & 0xFFFF) < 4) CVT_TRY(check_elements(ctx_, out.numbers.size(), "USD values"));
      }
      return {};
    }
    if (c == '{') {  // dictionary or time samples: keep the first sample
      ++pos_;
      bool first = true;
      for (;;) {
        const char d = peek();
        if (d == '}') {
          ++pos_;
          break;
        }
        if (d == '\0') return fail(Status::Decode, "unterminated dictionary in the USD file");
        if (d == ',') {
          ++pos_;
          continue;
        }
        const std::size_t before = pos_;
        UsdValue key;
        // key: number (time sample), or "type name" / string for dictionaries
        if (util::is_ascii_digit(d) || d == '-' || d == '.') {
          CVT_TRY(value(key, depth + 1));
          if (!eat(':')) return fail(Status::Decode, "malformed USD time samples");
          UsdValue sample;
          CVT_TRY(value(sample, depth + 1));
          if (first) {
            out = std::move(sample);
            first = false;
          }
          continue;
        }
        if (d == '"' || d == '\'') {
          CVT_TRY(value(key, depth + 1));
        } else {
          (void)word();  // type
          if (peek() == '"' || peek() == '\'') CVT_TRY(value(key, depth + 1));
          else (void)word();
        }
        if (eat('=') || eat(':')) {
          UsdValue ignored;
          CVT_TRY(value(ignored, depth + 1));
        }
        if (pos_ == before) return fail(Status::Decode, "malformed dictionary in the USD file");
      }
      return {};
    }
    const std::string_view token = word();
    if (token.empty()) {
      ++pos_;
      return fail(Status::Decode, "unexpected character in the USD file");
    }
    double number = 0;
    if (parse_double(token, number)) {
      out.numbers.push_back(number);
    } else {
      out.text = std::string(token);  // a bare token (None, true, ...)
      if (token == "true") out.numbers.push_back(1);
      else if (token == "false") out.numbers.push_back(0);
    }
    return {};
  }

  Result<void> metadata(std::map<std::string, std::string, std::less<>>& out) {
    if (!eat('(')) return {};
    for (int guard = 0;; ++guard) {
      if (guard > 100000) return fail(Status::Decode, "malformed USD metadata");
      const char c = peek();
      if (c == ')') {
        ++pos_;
        return {};
      }
      if (c == '\0') return fail(Status::Decode, "unterminated USD metadata");
      if (c == '"' || c == '\'') {  // documentation string
        CVT_TRY_ASSIGN(std::string doc, string_literal());
        out["doc"] = doc;
        continue;
      }
      if (c == ';' || c == ',') {
        ++pos_;
        continue;
      }
      std::string key(word());
      if (key == "prepend" || key == "append" || key == "add" || key == "delete" || key == "reorder") key = std::string(word());
      if (key == "dictionary" || key == "string" || key == "double" || key == "token" || key == "int" || key == "bool" || key == "float") {
        key = std::string(word());  // typed customData entry
      }
      if (key.empty()) {
        ++pos_;
        continue;
      }
      UsdValue v;
      if (eat('=')) CVT_TRY(value(v, 0));
      out[key] = v.text.empty() && !v.numbers.empty() ? format_number(v.numbers.front()) : v.text;
    }
  }

  Result<void> statement(UsdPrim& parent, int depth) {
    if (depth > 64) return fail(Status::Decode, "USD prims are nested too deeply");
    if (++statements_ > 5'000'000) return fail(Status::Limit, "the USD layer is too large");
    std::string_view first = word();
    if (first.empty()) {
      const char c = peek();
      if (c == '}' || c == '\0') return {};
      ++pos_;
      return fail(Status::Decode, "unexpected '{}' in the USD file", c);
    }
    if (first == "def" || first == "over" || first == "class") {
      auto prim = std::make_unique<UsdPrim>();
      prim->specifier = std::string(first);
      if (peek() != '"') prim->type = std::string(word());
      CVT_TRY_ASSIGN(prim->name, string_literal());
      prim->path = parent.path + "/" + prim->name;
      std::map<std::string, std::string, std::less<>> meta;
      if (peek() == '(') CVT_TRY(metadata(meta));
      if (const auto it = meta.find("references"); it != meta.end()) ctx_.warn("USD references to other files were not followed.");
      if (const auto it = meta.find("payload"); it != meta.end()) ctx_.warn("USD payloads in other files were not followed.");
      if (!eat('{')) return fail(Status::Decode, "USD prim '{}' has no body", prim->name);
      for (;;) {
        const char c = peek();
        if (c == '}') {
          ++pos_;
          break;
        }
        if (c == '\0') return fail(Status::Decode, "the USD file is truncated");
        CVT_TRY(statement(*prim, depth + 1));
      }
      parent.children.push_back(std::move(prim));
      return {};
    }
    if (first == "variantSet") {
      // variantSet "name" = { "a" { ... } ... }: skip
      CVT_TRY(skip_block());
      ctx_.warn("USD variant sets were ignored.");
      return {};
    }
    // Property: [qualifiers] type name [= value] [(meta)]  |  rel name [= target]
    std::string type(first);
    while (type == "uniform" || type == "custom" || type == "varying" || type == "prepend" || type == "append" ||
           type == "delete" || type == "add" || type == "reorder" || type == "config") {
      type = std::string(word());
    }
    const bool relationship = type == "rel";
    std::string name(word());
    if (name.empty()) return fail(Status::Decode, "malformed USD property");
    UsdAttribute attribute;
    attribute.type = type;
    if (eat('=')) CVT_TRY(value(attribute.value, 0));
    std::map<std::string, std::string, std::less<>> meta;
    if (peek() == '(') CVT_TRY(metadata(meta));
    attribute.meta = std::move(meta);
    if (relationship) {
      parent.relationships[name] = attribute.value.text;
      return {};
    }
    if (name.ends_with(".connect")) {
      const std::string base = name.substr(0, name.size() - 8);
      parent.attributes[base].connection = attribute.value.text;
      if (parent.attributes[base].type.empty()) parent.attributes[base].type = type;
      return {};
    }
    if (name.ends_with(".timeSamples")) name.resize(name.size() - 12);
    auto& slot = parent.attributes[name];
    const std::string connection = slot.connection;
    slot = std::move(attribute);
    slot.connection = connection;
    return {};
  }

  Result<void> skip_block() {
    while (pos_ < text_.size() && text_[pos_] != '{') ++pos_;
    int level = 0;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '{') ++level;
      else if (c == '}' && --level == 0) return {};
    }
    return fail(Status::Decode, "unterminated block in the USD file");
  }

  std::string_view text_;
  const Context& ctx_;
  std::size_t pos_ = 0;
  std::size_t statements_ = 0;
};

// ------------------------------------------------------------------------- interpretation

class UsdScene {
 public:
  UsdScene(const UsdPrim& root, const Context& ctx, std::function<std::optional<Bytes>(std::string_view)> files)
      : root_(root), ctx_(ctx), files_(std::move(files)) {
    index(root_);
  }

  Result<ir::Mesh> build() {
    CVT_TRY(visit(root_, {}, 0));
    return std::move(mesh_);
  }

 private:
  void index(const UsdPrim& prim) {
    by_path_[prim.path] = &prim;
    for (const auto& c : prim.children) index(*c);
  }
  const UsdPrim* prim_at(std::string_view path) const {
    std::string_view p = path;
    if (const std::size_t dot = p.find('.'); dot != std::string_view::npos) p = p.substr(0, dot);
    const auto it = by_path_.find(std::string(p));
    return it == by_path_.end() ? nullptr : it->second;
  }

  static ir::Mat4 op_matrix(const std::string& op, const UsdAttribute& a) {
    const std::vector<double>& v = a.value.numbers;
    const auto f = [&](std::size_t i, double fallback) { return static_cast<float>(i < v.size() ? v[i] : fallback); };
    if (op.find("transform") != std::string::npos && v.size() >= 16) {
      ir::Mat4 m;
      for (int i = 0; i < 16; ++i) m.m[i] = static_cast<float>(v[static_cast<std::size_t>(i)]);  // row vectors: same layout
      return m;
    }
    if (op.find("translate") != std::string::npos) return translation(f(0, 0), f(1, 0), f(2, 0));
    if (op.find("scale") != std::string::npos) {
      return v.size() >= 3 ? scaling(f(0, 1), f(1, 1), f(2, 1)) : scaling(f(0, 1), f(0, 1), f(0, 1));
    }
    if (op.find("orient") != std::string::npos && v.size() >= 4) return rotation_quaternion(f(1, 0), f(2, 0), f(3, 0), f(0, 1));
    const std::size_t at = op.find("rotate");
    if (at != std::string::npos) {
      std::string axes = op.substr(at + 6);
      if (const std::size_t colon = axes.find(':'); colon != std::string::npos) axes.resize(colon);
      if (axes.size() == 1) {
        const float angle = f(0, 0);
        return rotation(angle, axes == "X" ? 1.0f : 0.0f, axes == "Y" ? 1.0f : 0.0f, axes == "Z" ? 1.0f : 0.0f);
      }
      ir::Mat4 m;
      for (std::size_t k = 0; k < axes.size() && k < 3; ++k) {  // first axis applied first
        const char axis = axes[k];
        const std::size_t component = axis == 'X' ? 0 : axis == 'Y' ? 1 : 2;
        m = rotation(f(component, 0), axis == 'X' ? 1.0f : 0.0f, axis == 'Y' ? 1.0f : 0.0f, axis == 'Z' ? 1.0f : 0.0f) * m;
      }
      return m;
    }
    return {};
  }

  ir::Mat4 local_transform(const UsdPrim& prim) const {
    const auto order = prim.attributes.find("xformOpOrder");
    if (order == prim.attributes.end()) return {};
    std::vector<std::string> ops = order->second.value.texts;
    if (ops.empty() && !order->second.value.text.empty()) ops.push_back(order->second.value.text);
    ir::Mat4 m;
    for (const std::string& token : ops) {
      std::string op = token;
      bool invert = false;
      if (op.starts_with("!invert!")) {
        invert = true;
        op = op.substr(8);
      }
      if (op == "!resetXformStack!") continue;
      const auto it = prim.attributes.find(op);
      if (it == prim.attributes.end()) continue;
      ir::Mat4 piece = op_matrix(op, it->second);
      if (invert) piece = inverse(piece);
      m = m * piece;
    }
    return m;
  }

  Result<void> visit(const UsdPrim& prim, const ir::Mat4& parent, int depth) {
    if (depth > 64) return fail(Status::Decode, "USD prims are nested too deeply");
    if (prim.specifier == "class") return {};
    const ir::Mat4 world = parent * local_transform(prim);
    if (prim.type == "Mesh") CVT_TRY(mesh(prim, world));
    for (const auto& c : prim.children) {
      if (c->type == "Material" || c->type == "Shader" || c->type == "GeomSubset") continue;
      CVT_TRY(visit(*c, world, depth + 1));
    }
    return {};
  }

  std::string material(std::string_view path) {
    const UsdPrim* m = prim_at(path);
    if (m == nullptr) return {};
    if (const auto it = names_.find(m); it != names_.end()) return it->second;
    ir::Material out;
    out.name = m->name;
    for (int k = 2; mesh_.find_material(out.name) != nullptr; ++k) out.name = std::format("{}_{}", m->name, k);
    out.base_color = {0.18f, 0.18f, 0.18f, 1.0f};
    out.roughness = 0.5f;
    // The surface shader: outputs:surface.connect, else the first UsdPreviewSurface child.
    const UsdPrim* shader = nullptr;
    if (const auto it = m->attributes.find("outputs:surface"); it != m->attributes.end()) shader = prim_at(it->second.connection);
    for (const auto& c : m->children) {
      if (shader == nullptr && c->type == "Shader") shader = c.get();
    }
    if (shader != nullptr) {
      const auto get = [&](std::string_view input) -> const UsdAttribute* {
        const auto it = shader->attributes.find(input);
        return it == shader->attributes.end() ? nullptr : &it->second;
      };
      if (const UsdAttribute* d = get("inputs:diffuseColor")) {
        if (d->value.numbers.size() >= 3) {
          out.base_color = {static_cast<float>(d->value.numbers[0]), static_cast<float>(d->value.numbers[1]),
                            static_cast<float>(d->value.numbers[2]), 1.0f};
        }
        if (!d->connection.empty()) {
          if (const UsdPrim* tex = prim_at(d->connection)) {
            const auto file = tex->attributes.find("inputs:file");
            if (file != tex->attributes.end() && !file->second.value.text.empty()) {
              ir::Texture texture;
              texture.uri = file->second.value.text;
              texture.name = std::string(base_name(texture.uri));
              if (files_) {
                if (auto bytes = files_(texture.uri)) texture.data = std::move(*bytes);
              }
              texture.mime = image_mime(texture.data, texture.uri);
              if (texture.data.empty()) ctx_.warn(std::format("Texture '{}' is a separate file and was not included.", texture.uri));
              out.base_color_texture = static_cast<int>(mesh_.textures.size());
              out.base_color = {1, 1, 1, 1};
              mesh_.textures.push_back(std::move(texture));
            }
          }
        }
      }
      const auto scalar = [&](std::string_view input, float fallback) {
        const UsdAttribute* a = get(input);
        return a != nullptr && !a->value.numbers.empty() ? static_cast<float>(a->value.numbers[0]) : fallback;
      };
      out.metallic = std::clamp(scalar("inputs:metallic", 0.0f), 0.0f, 1.0f);
      out.roughness = std::clamp(scalar("inputs:roughness", 0.5f), 0.0f, 1.0f);
      out.base_color.a = std::clamp(scalar("inputs:opacity", 1.0f), 0.0f, 1.0f);
      if (const UsdAttribute* e = get("inputs:emissiveColor"); e != nullptr && e->value.numbers.size() >= 3) {
        out.emissive = {static_cast<float>(e->value.numbers[0]), static_cast<float>(e->value.numbers[1]), static_cast<float>(e->value.numbers[2])};
      }
    }
    names_[m] = out.name;
    mesh_.materials.push_back(std::move(out));
    return names_[m];
  }

  Result<void> mesh(const UsdPrim& prim, const ir::Mat4& world) {
    const auto attr = [&](std::string_view name) -> const UsdAttribute* {
      const auto it = prim.attributes.find(name);
      return it == prim.attributes.end() ? nullptr : &it->second;
    };
    const UsdAttribute* points = attr("points");
    const UsdAttribute* counts = attr("faceVertexCounts");
    const UsdAttribute* indices = attr("faceVertexIndices");
    if (points == nullptr || counts == nullptr || indices == nullptr) return {};
    const std::vector<double>& p = points->value.numbers;
    const std::size_t n = p.size() / 3;
    const std::vector<double>& fc = counts->value.numbers;
    const std::vector<double>& fi = indices->value.numbers;
    CVT_TRY(check_elements(ctx_, fi.size(), "face corners"));
    // Per-corner sources: (values, width, interpolation, indices).
    struct Source {
      const std::vector<double>* values = nullptr;
      std::vector<double> indices;
      std::size_t width = 0;
      std::string interpolation;
      [[nodiscard]] std::int64_t pick(std::size_t corner, std::int64_t point, std::size_t face) const {
        std::int64_t element = 0;
        if (interpolation == "faceVarying") element = static_cast<std::int64_t>(corner);
        else if (interpolation == "uniform") element = static_cast<std::int64_t>(face);
        else if (interpolation == "constant") element = 0;
        else element = point;  // vertex / varying
        if (!indices.empty()) {
          if (element < 0 || static_cast<std::size_t>(element) >= indices.size()) return -1;
          const std::uint64_t mapped = file_index(indices[static_cast<std::size_t>(element)]);
          if (mapped == invalid_index) return -1;
          element = static_cast<std::int64_t>(mapped);
        }
        if (element < 0 || static_cast<std::size_t>(element + 1) * width > values->size()) return -1;
        return element;
      }
    };
    const auto source = [&](std::string_view name, std::size_t width) -> std::optional<Source> {
      const UsdAttribute* a = attr(name);
      if (a == nullptr || a->value.numbers.empty()) return std::nullopt;
      Source s;
      s.values = &a->value.numbers;
      s.width = width;
      const auto it = a->meta.find("interpolation");
      s.interpolation = it == a->meta.end() ? "vertex" : it->second;
      if (const UsdAttribute* idx = attr(std::string(name) + ":indices")) s.indices = idx->value.numbers;
      return s;
    };
    std::optional<Source> normals = source("primvars:normals", 3);
    if (!normals) normals = source("normals", 3);
    std::optional<Source> uvs;
    std::optional<Source> colors;
    for (const auto& [name, a] : prim.attributes) {
      if (!name.starts_with("primvars:") || name.ends_with(":indices")) continue;
      if (!uvs && (a.type == "texCoord2f[]" || a.type == "float2[]" || a.type == "texCoord2d[]")) uvs = source(name, 2);
      if (a.type == "color3f[]" && (!colors || name == "primvars:displayColor")) colors = source(name, 3);
    }
    if (colors && colors->interpolation == "constant" && colors->values->size() < 3) colors.reset();
    const bool left_handed = [&] {
      const UsdAttribute* o = attr("orientation");
      return o != nullptr && o->value.text == "leftHanded";
    }();
    // Per-face materials: the prim's binding, overridden by GeomSubsets.
    std::vector<std::string> face_material(fc.size());
    if (const auto it = prim.relationships.find("material:binding"); it != prim.relationships.end()) {
      std::fill(face_material.begin(), face_material.end(), material(it->second));
    }
    for (const auto& c : prim.children) {
      if (c->type != "GeomSubset") continue;
      const auto rel = c->relationships.find("material:binding");
      const auto idx = c->attributes.find("indices");
      if (rel == c->relationships.end() || idx == c->attributes.end()) continue;
      const std::string name = material(rel->second);
      for (const double f : idx->second.value.numbers) {
        if (f >= 0 && f < static_cast<double>(face_material.size())) face_material[static_cast<std::size_t>(f)] = name;
      }
    }
    ir::Mesh part;
    std::map<std::array<std::int64_t, 4>, std::uint32_t> made;
    std::size_t corner = 0;
    for (std::size_t f = 0; f < fc.size(); ++f) {
      const std::uint64_t count = file_index(fc[f]);
      if (count > fi.size() - std::min(corner, fi.size())) return fail(Status::Decode, "USD face counts exceed the index list");
      std::vector<std::uint32_t> face;
      for (std::size_t k = 0; k < count; ++k, ++corner) {
        const double pv = fi[corner];
        if (!(pv >= 0) || pv >= static_cast<double>(n)) return fail(Status::Decode, "a USD face refers to missing point {}", pv);
        const auto point = static_cast<std::int64_t>(pv);
        const std::array<std::int64_t, 4> key = {point, normals ? normals->pick(corner, point, f) : -1, uvs ? uvs->pick(corner, point, f) : -1,
                                                 colors ? colors->pick(corner, point, f) : -1};
        const auto [it, inserted] = made.emplace(key, static_cast<std::uint32_t>(part.positions.size()));
        if (inserted) {
          const auto at = static_cast<std::size_t>(point) * 3;
          part.positions.push_back({static_cast<float>(p[at]), static_cast<float>(p[at + 1]), static_cast<float>(p[at + 2])});
          const auto get = [](const std::optional<Source>& s, std::int64_t e, std::size_t k) {
            return e < 0 ? 0.0f : static_cast<float>((*s->values)[static_cast<std::size_t>(e) * s->width + k]);
          };
          if (normals) part.normals.push_back({get(normals, key[1], 0), get(normals, key[1], 1), get(normals, key[1], 2)});
          if (uvs) part.texcoords.push_back({get(uvs, key[2], 0), get(uvs, key[2], 1)});
          if (colors) {
            part.colors.push_back(key[3] < 0 ? ir::Color{} : ir::Color{get(colors, key[3], 0), get(colors, key[3], 1), get(colors, key[3], 2), 1.0f});
          }
        }
        face.push_back(it->second);
      }
      if (count < 3) continue;
      if (left_handed) std::reverse(face.begin() + 1, face.end());
      part.indices.insert(part.indices.end(), face.begin(), face.end());
      part.face_sizes.push_back(static_cast<std::uint32_t>(count));
      if (part.groups.empty() || part.groups.back().material != face_material[f]) {
        part.groups.push_back({prim.name, face_material[f], part.face_sizes.size() - 1, 0});
      }
      part.groups.back().face_count++;
    }
    if (part.indices.empty()) return {};
    transform(part, world);
    const std::size_t first = mesh_.face_count();
    const auto materials = std::move(mesh_.materials);
    const auto textures = std::move(mesh_.textures);
    append(mesh_, part);
    mesh_.materials = materials;
    mesh_.textures = textures;
    mesh_.objects.push_back({prim.name, first, mesh_.face_count() - first, world});
    return check_elements(ctx_, mesh_.positions.size(), "vertices");
  }

  const UsdPrim& root_;
  const Context& ctx_;
  std::function<std::optional<Bytes>(std::string_view)> files_;
  std::map<std::string, const UsdPrim*> by_path_;
  std::map<const UsdPrim*, std::string> names_;
  ir::Mesh mesh_;
};


Result<ir::Mesh> read_layer(std::string_view text, const ReadContext& ctx, std::function<std::optional<Bytes>(std::string_view)> files) {
  UsdaParser parser(text, ctx);
  std::map<std::string, std::string, std::less<>> meta;
  CVT_TRY_ASSIGN(const std::unique_ptr<UsdPrim> root, parser.parse(meta));
  UsdScene scene(*root, ctx, std::move(files));
  CVT_TRY_ASSIGN(ir::Mesh mesh, scene.build());
  if (mesh.positions.empty()) return fail(Status::Decode, "the USD layer contains no meshes");
  if (mesh.objects.size() == 1 && mesh.objects[0].transform.is_identity()) mesh.objects.clear();
  double meters = 0.01;  // USD's default unit is the centimetre
  if (const auto it = meta.find("metersPerUnit"); it != meta.end()) {
    double v = 0;
    if (parse_double(it->second, v) && v > 0) meters = v;
  }
  mesh.unit_meters = meters;
  if (const auto it = meta.find("doc"); it != meta.end() && !it->second.empty()) mesh.metadata["comment"] = it->second;
  const auto up = meta.find("upAxis");
  if (up != meta.end() && up->second == "Z") {
    mesh.source_up_axis = "Z";
    convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  } else {
    mesh.source_up_axis = "Y";
  }
  return mesh;
}

Result<AnyIR> read_usda(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Mesh mesh, read_layer(as_chars(ctx.input), ctx, {}));
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

Result<AnyIR> read_usdz(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  std::optional<std::size_t> layer;
  for (std::size_t i = 0; i < zip.entries().size() && !layer; ++i) {
    const std::string& name = zip.entries()[i].name;
    if (util::iends_with(name, ".usda") || util::iends_with(name, ".usdc") || util::iends_with(name, ".usd")) layer = i;
  }
  if (!layer) return fail(Status::Decode, "the USDZ package has no USD layer");
  const std::string layer_name = zip.entries()[*layer].name;
  CVT_TRY_ASSIGN(const Bytes data, zip.read(*layer));
  if (as_chars(data).starts_with("PXR-USDC")) {
    return fail(Status::Decode, "this USDZ package stores its scene as binary USD (usdc), which is not supported; "
                "export it with a text (usda) root layer");
  }
  const auto files = [&](std::string_view uri) -> std::optional<Bytes> {
    std::string path = resolve_relative(layer_name, uri);
    if (auto index = zip.find(path)) {
      if (auto bytes = zip.read(*index)) return std::move(*bytes);
    }
    return std::nullopt;
  };
  CVT_TRY_ASSIGN(ir::Mesh mesh, read_layer(as_chars(data), ctx, files));
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// ------------------------------------------------------------------------- writing

constexpr WriterTraits usd_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.objects = true;
  traits.up = WriterTraits::Up::Y;
  return traits;
}

std::string usd_name(std::string_view text, std::string_view fallback, std::map<std::string, int>& used) {
  std::string out;
  for (const char c : text) out.push_back(util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' ? c : '_');
  if (out.empty() || util::is_ascii_digit(out.front())) out = std::string(fallback) + out;
  std::string unique = out;
  for (int k = 2; used.contains(unique); ++k) unique = std::format("{}_{}", out, k);
  used[unique] = 1;
  return unique;
}

template <class T, class Fn>
std::string tuple_list(const std::vector<T>& values, Fn&& components, int precision) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ", ";
    out += '(';
    const auto parts = components(values[i]);
    for (std::size_t k = 0; k < parts.size(); ++k) {
      if (k > 0) out += ", ";
      append_number(out, parts[k], precision);
    }
    out += ')';
  }
  return out + "]";
}

// Writes the usda layer; texture files (name -> bytes) are collected for usdz.
Result<std::string> usda_text(const ir::Mesh& source, WriteContext& ctx, std::vector<std::pair<std::string, Bytes>>* textures) {
  ir::Mesh mesh = source;
  const UnitChoice unit = resolve_unit(mesh, ctx, 1.0);
  if (unit.scale != 1.0f) {
    scale(mesh, unit.scale);
    for (ir::MeshObject& o : mesh.objects) {
      for (int r = 0; r < 3; ++r) o.transform.m[12 + r] *= unit.scale;
    }
  }
  const int precision = write_precision(ctx);
  std::string out = "#usda 1.0\n(\n    defaultPrim = \"Root\"\n";
  if (!options::resolved_strip_metadata(ctx.options)) {
    if (const auto it = mesh.metadata.find("comment"); it != mesh.metadata.end()) {
      std::string doc;
      for (const char c : it->second) doc += c == '"' ? '\'' : (c == '\n' ? ' ' : c);
      out += std::format("    doc = \"{}\"\n", doc);
    }
  }
  out += std::format("    metersPerUnit = {}\n    upAxis = \"Y\"\n)\n\ndef Xform \"Root\"\n{{\n", format_number(unit.meters));
  std::map<std::string, int> used;
  std::map<std::string, std::string> material_path;
  std::vector<std::string> texture_file(mesh.textures.size());
  for (std::size_t t = 0; t < mesh.textures.size(); ++t) {
    const ir::Texture& texture = mesh.textures[t];
    std::string stem = usd_name(std::string(base_name(texture.name.empty() ? texture.uri : texture.name)), "texture", used);
    const std::string mime = texture.mime.empty() ? image_mime(texture.data, texture.uri) : texture.mime;
    texture_file[t] = std::format("textures/{}.{}", stem, image_extension(mime));
    if (textures != nullptr && !texture.data.empty() && (mime == "image/png" || mime == "image/jpeg")) {
      textures->emplace_back(texture_file[t], texture.data);
    } else if (textures != nullptr) {
      ctx.warn(std::format("Texture '{}' could not be packaged (USDZ holds PNG and JPEG images).", texture.name));
    }
  }
  if (textures == nullptr && !mesh.textures.empty()) ctx.warn("A .usda file refers to textures by file name; they were not embedded (use .usdz).");
  for (const ir::Material& m : mesh.materials) material_path[m.name] = "/Root/Materials/" + usd_name(m.name, "Material", used);

  const std::vector<ObjectPart> parts = split_objects(mesh, true);
  for (std::size_t p = 0; p < parts.size(); ++p) {
    CVT_TRY(ctx.checkpoint(0.8 + 0.15 * static_cast<double>(p) / static_cast<double>(parts.size())));
    const ir::Mesh& part = parts[p].mesh;
    if (part.indices.empty()) continue;
    const std::string name = usd_name(parts[p].name, "Object", used);
    out += std::format("    def Xform \"{}\"\n    {{\n", name);
    if (!parts[p].transform.is_identity()) {
      out += "        matrix4d xformOp:transform = (";
      for (int row = 0; row < 4; ++row) {
        out += row == 0 ? " (" : ", (";
        for (int col = 0; col < 4; ++col) {
          if (col > 0) out += ", ";
          append_number(out, parts[p].transform.m[row * 4 + col]);  // USD: row vectors
        }
        out += ')';
      }
      out += " )\n        uniform token[] xformOpOrder = [\"xformOp:transform\"]\n";
    }
    out += "\n        def Mesh \"Mesh\"\n        {\n";
    const std::vector<std::size_t> offsets = face_offsets(part);
    out += "            int[] faceVertexCounts = [";
    for (std::size_t f = 0; f + 1 < offsets.size(); ++f) out += std::format("{}{}", f == 0 ? "" : ", ", offsets[f + 1] - offsets[f]);
    out += "]\n            int[] faceVertexIndices = [";
    for (std::size_t c = 0; c < part.indices.size(); ++c) out += std::format("{}{}", c == 0 ? "" : ", ", part.indices[c]);
    out += "]\n";
    if (!part.normals.empty()) {
      out += std::format("            normal3f[] normals = {} (\n                interpolation = \"vertex\"\n            )\n",
                         tuple_list(part.normals, [](const ir::Vec3& v) { return std::array<float, 3>{v.x, v.y, v.z}; }, precision == 0 ? 7 : precision));
    }
    out += std::format("            point3f[] points = {}\n",
                       tuple_list(part.positions, [](const ir::Vec3& v) { return std::array<float, 3>{v.x, v.y, v.z}; }, precision));
    if (!part.colors.empty()) {
      out += std::format("            color3f[] primvars:displayColor = {} (\n                interpolation = \"vertex\"\n            )\n",
                         tuple_list(part.colors, [](const ir::Color& c) { return std::array<float, 3>{c.r, c.g, c.b}; }, 7));
    }
    if (!part.texcoords.empty()) {
      out += std::format("            texCoord2f[] primvars:st = {} (\n                interpolation = \"vertex\"\n            )\n",
                         tuple_list(part.texcoords, [](const ir::Vec2& v) { return std::array<float, 2>{v.x, v.y}; }, precision));
    }
    out += "            uniform token subdivisionScheme = \"none\"\n";
    // Materials: one binding, or GeomSubsets for several.
    std::vector<std::string> face_material(part.face_count());
    for (const ir::MeshGroup& g : part.groups) {
      for (std::size_t f = g.first_face; f < g.first_face + g.face_count && f < face_material.size(); ++f) face_material[f] = g.material;
    }
    std::vector<std::string> distinct;
    for (const std::string& m : face_material) {
      if (material_path.contains(m) && std::find(distinct.begin(), distinct.end(), m) == distinct.end()) distinct.push_back(m);
    }
    if (distinct.size() == 1 && std::all_of(face_material.begin(), face_material.end(), [&](const std::string& m) { return m == distinct[0]; })) {
      out += std::format("            rel material:binding = <{}>\n", material_path[distinct[0]]);
    } else {
      for (std::size_t k = 0; k < distinct.size(); ++k) {
        out += std::format("\n            def GeomSubset \"subset_{}\"\n            {{\n                uniform token elementType = \"face\"\n"
                           "                uniform token familyName = \"materialBind\"\n                int[] indices = [",
                           k);
        bool first = true;
        for (std::size_t f = 0; f < face_material.size(); ++f) {
          if (face_material[f] != distinct[k]) continue;
          out += std::format("{}{}", first ? "" : ", ", f);
          first = false;
        }
        out += std::format("]\n                rel material:binding = <{}>\n            }}\n", material_path[distinct[k]]);
      }
    }
    out += "        }\n    }\n\n";
  }
  if (!mesh.materials.empty()) {
    out += "    def Scope \"Materials\"\n    {\n";
    for (const ir::Material& m : mesh.materials) {
      const std::string path = material_path[m.name];
      const std::string name(base_name(path));
      out += std::format("        def Material \"{0}\"\n        {{\n            token outputs:surface.connect = <{1}/Surface.outputs:surface>\n\n"
                         "            def Shader \"Surface\"\n            {{\n                uniform token info:id = \"UsdPreviewSurface\"\n",
                         name, path);
      const bool textured = m.base_color_texture >= 0 && !texture_file[static_cast<std::size_t>(m.base_color_texture)].empty();
      if (textured) out += std::format("                color3f inputs:diffuseColor.connect = <{}/Texture.outputs:rgb>\n", path);
      else out += std::format("                color3f inputs:diffuseColor = ({}, {}, {})\n", format_number(m.base_color.r, 7), format_number(m.base_color.g, 7), format_number(m.base_color.b, 7));
      out += std::format("                color3f inputs:emissiveColor = ({}, {}, {})\n                float inputs:metallic = {}\n"
                         "                float inputs:opacity = {}\n                float inputs:roughness = {}\n"
                         "                token outputs:surface\n            }}\n",
                         format_number(m.emissive.x, 7), format_number(m.emissive.y, 7), format_number(m.emissive.z, 7), format_number(m.metallic, 7),
                         format_number(m.base_color.a, 7), format_number(m.roughness, 7));
      if (textured) {
        out += std::format("\n            def Shader \"Texture\"\n            {{\n                uniform token info:id = \"UsdUVTexture\"\n"
                           "                asset inputs:file = @{0}@\n                float2 inputs:st.connect = <{1}/Reader.outputs:result>\n"
                           "                float3 outputs:rgb\n            }}\n\n            def Shader \"Reader\"\n            {{\n"
                           "                uniform token info:id = \"UsdPrimvarReader_float2\"\n                token inputs:varname = \"st\"\n"
                           "                float2 outputs:result\n            }}\n",
                           texture_file[static_cast<std::size_t>(m.base_color_texture)], path);
      }
      out += "        }\n";
    }
    out += "    }\n";
  }
  out += "}\n";
  return out;
}

Result<Bytes> write_usda(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, usd_traits()));
  CVT_TRY_ASSIGN(const std::string text, usda_text(mesh, ctx, nullptr));
  CVT_TRY(ctx.limits.check_output(text.size()));
  return to_bytes(text);
}

// A stored (uncompressed) ZIP with every file's data aligned to 64 bytes, as
// USDZ requires; the alignment is made with an extra field in each local header.
Bytes aligned_zip(const std::vector<std::pair<std::string, Bytes>>& files) {
  util::ByteWriter out;
  struct Entry {
    std::string name;
    std::uint32_t crc;
    std::uint32_t size;
    std::uint32_t offset;
  };
  std::vector<Entry> entries;
  for (const auto& [name, data] : files) {
    const auto offset = static_cast<std::uint32_t>(out.size());
    const std::size_t header = 30 + name.size();
    std::size_t padding = (64 - (offset + header + 4) % 64) % 64;
    const std::uint32_t crc = util::crc32(data);
    out.u32le(0x04034B50).u16le(20).u16le(0).u16le(0).u16le(0).u16le(0x21);  // version, flags, stored, time, date (1980-01-01)
    out.u32le(crc).u32le(static_cast<std::uint32_t>(data.size())).u32le(static_cast<std::uint32_t>(data.size()));
    out.u16le(static_cast<std::uint16_t>(name.size())).u16le(static_cast<std::uint16_t>(padding + 4));
    out.text(name);
    out.u16le(0x1986).u16le(static_cast<std::uint16_t>(padding)).zeros(padding);  // padding extra field
    out.bytes(data);
    entries.push_back({name, crc, static_cast<std::uint32_t>(data.size()), offset});
  }
  const auto directory = static_cast<std::uint32_t>(out.size());
  for (const Entry& e : entries) {
    out.u32le(0x02014B50).u16le(20).u16le(20).u16le(0).u16le(0).u16le(0).u16le(0x21);
    out.u32le(e.crc).u32le(e.size).u32le(e.size).u16le(static_cast<std::uint16_t>(e.name.size())).u16le(0).u16le(0).u16le(0).u16le(0);
    out.u32le(0).u32le(e.offset).text(e.name);
  }
  const auto directory_size = static_cast<std::uint32_t>(out.size() - directory);
  out.u32le(0x06054B50).u16le(0).u16le(0).u16le(static_cast<std::uint16_t>(entries.size())).u16le(static_cast<std::uint16_t>(entries.size()));
  out.u32le(directory_size).u32le(directory).u16le(0);
  return out.take();
}

Result<Bytes> write_usdz(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, usd_traits()));
  std::vector<std::pair<std::string, Bytes>> textures;
  CVT_TRY_ASSIGN(const std::string text, usda_text(mesh, ctx, &textures));
  std::vector<std::pair<std::string, Bytes>> files;
  files.emplace_back("model.usda", to_bytes(text));
  for (auto& t : textures) files.push_back(std::move(t));
  Bytes out = aligned_zip(files);
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

int sniff_usdz(ByteView data, std::string_view /*filename*/) {
  if (data.size() < 30 || !as_chars(data).starts_with("PK\x03\x04")) return 0;
  util::ByteReader r(data);
  r.skip(26);
  const std::uint16_t name_length = r.u16le();
  r.skip(2);
  const std::string_view name = r.chars(name_length);
  return r.ok() && (util::iends_with(name, ".usda") || util::iends_with(name, ".usdc") || util::iends_with(name, ".usd")) ? 95 : 0;
}

}  // namespace

void register_usd(Registry& registry) {
  FormatInfo usda;
  usda.id = "usda";
  usda.name = "USD scene (text)";
  usda.description = "Pixar Universal Scene Description in text form.";
  usda.category = Category::Model3D;
  usda.extensions = {"usda", "usd"};
  usda.mime = "model/vnd.usda";
  usda.popularity = 30;
  usda.signatures = {Signature::at(0, "'#usda '")};
  registry.add_format(std::move(usda));
  Codec usda_codec;
  usda_codec.format_id = "usda";
  usda_codec.kind = Kind::Mesh;
  usda_codec.read = read_usda;
  usda_codec.write = write_usda;
  usda_codec.write_options = mesh_write_options(usd_traits());
  usda_codec.write_options.push_back(unit_option());
  registry.add_codec(std::move(usda_codec));

  FormatInfo usdz;
  usdz.id = "usdz";
  usdz.name = "USDZ package";
  usdz.description = "USD scene packed with its textures (used by Apple AR Quick Look).";
  usdz.category = Category::Model3D;
  usdz.extensions = {"usdz"};
  usdz.mime = "model/vnd.usdz+zip";
  usdz.popularity = 35;
  usdz.sniff = sniff_usdz;
  registry.add_format(std::move(usdz));
  Codec usdz_codec;
  usdz_codec.format_id = "usdz";
  usdz_codec.kind = Kind::Mesh;
  usdz_codec.read = read_usdz;
  usdz_codec.write = write_usdz;
  usdz_codec.write_options = mesh_write_options(usd_traits());
  usdz_codec.write_options.push_back(unit_option());
  registry.add_codec(std::move(usdz_codec));

  // Binary crate files are recognised so users get a clear message.
  FormatInfo usdc;
  usdc.id = "usdc";
  usdc.name = "USD scene (binary crate)";
  usdc.description = "Pixar USD binary crate layer (detection only; not readable).";
  usdc.category = Category::Model3D;
  usdc.extensions = {"usdc"};
  usdc.mime = "model/vnd.usd";
  usdc.popularity = 20;
  usdc.signatures = {Signature::at(0, "'PXR-USDC'")};
  registry.add_format(std::move(usdc));
}

}  // namespace cvt::mesh
