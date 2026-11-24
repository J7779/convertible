// Convertible core - mesh family: Autodesk FBX (binary 7.x and ASCII read,
// ASCII 7.4 write).
//
// Both encodings hold the same node tree: named records with typed
// properties and children. The binary form stores arrays raw or
// zlib-deflated. The scene is assembled from Objects (Model, Geometry,
// Material, Texture, Video) and Connections; model transforms follow the
// full FBX chain (translation, offsets, pivots, pre/post rotation, rotation
// order, scaling, geometric transform), and GlobalSettings give the up
// axis and the unit (centimetres per unit).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

struct FbxProperty {
  char type = 0;            // Y C I F D L S R, or f d l i b for arrays
  std::int64_t integer = 0;
  double number = 0;
  std::string text;         // S and R
  std::vector<double> numbers;  // float arrays
  std::vector<std::int64_t> integers;  // integer arrays
  [[nodiscard]] bool is_array() const { return type == 'f' || type == 'd' || type == 'l' || type == 'i' || type == 'b'; }
  [[nodiscard]] double as_double() const {
    return type == 'F' || type == 'D' ? number : static_cast<double>(integer);
  }
  [[nodiscard]] std::int64_t as_int() const {
    return type == 'F' || type == 'D' ? static_cast<std::int64_t>(number) : integer;
  }
};

struct FbxNode {
  std::string name;
  std::vector<FbxProperty> props;
  std::vector<FbxNode> children;
  [[nodiscard]] const FbxNode* child(std::string_view n) const {
    for (const FbxNode& c : children) {
      if (c.name == n) return &c;
    }
    return nullptr;
  }
};

constexpr int max_depth = 64;

// --------------------------------------------------------------------------- binary

class BinaryParser {
 public:
  BinaryParser(ByteView data, const ReadContext& ctx) : data_(data), ctx_(ctx) {}

  Result<FbxNode> parse() {
    util::ByteReader r(data_);
    if (!r.consume(std::string_view("Kaydara FBX Binary  \0", 21))) return fail(Status::Decode, "not a binary FBX file");
    r.skip(2);
    version_ = r.u32le();
    if (!r.ok()) return fail(Status::Decode, "the FBX header is truncated");
    if (version_ < 7000 || version_ > 7999) return fail(Status::Decode, "FBX version {} is not supported (7.x only)", version_);
    FbxNode root;
    std::size_t pos = r.position();
    for (;;) {
      FbxNode node;
      bool null_record = false;
      CVT_TRY(parse_node(pos, node, null_record, 0));
      if (null_record) break;
      root.children.push_back(std::move(node));
      if (pos >= data_.size()) break;
    }
    return root;
  }

 private:
  Result<void> parse_node(std::size_t& pos, FbxNode& node, bool& null_record, int depth) {
    if (depth > max_depth) return fail(Status::Decode, "FBX nodes are nested too deeply");
    if (++nodes_ > 5'000'000) return fail(Status::Limit, "the FBX file has too many nodes");
    util::ByteReader r(data_);
    r.seek(pos);
    const bool wide = version_ >= 7500;
    const std::uint64_t end = wide ? r.u64le() : r.u32le();
    const std::uint64_t count = wide ? r.u64le() : r.u32le();
    const std::uint64_t list_length = wide ? r.u64le() : r.u32le();
    const std::uint8_t name_length = r.u8();
    if (!r.ok()) return fail(Status::Decode, "an FBX node record is truncated");
    if (end == 0 && count == 0 && list_length == 0 && name_length == 0) {
      null_record = true;
      pos = r.position();
      return {};
    }
    if (end > data_.size() || end <= pos) return fail(Status::Decode, "an FBX node ends outside the file");
    node.name = std::string(r.chars(name_length));
    const std::size_t props_start = r.position();
    if (list_length > end - props_start) return fail(Status::Decode, "FBX properties run past their node");
    if (count > list_length) return fail(Status::Decode, "FBX property count is implausible");
    node.props.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
      FbxProperty prop;
      CVT_TRY(parse_property(r, prop));
      node.props.push_back(std::move(prop));
    }
    if (!r.ok() || r.position() > props_start + list_length) return fail(Status::Decode, "malformed FBX properties");
    std::size_t child = props_start + static_cast<std::size_t>(list_length);
    while (child < end) {
      FbxNode c;
      bool is_null = false;
      CVT_TRY(parse_node(child, c, is_null, depth + 1));
      if (is_null) break;
      node.children.push_back(std::move(c));
    }
    pos = static_cast<std::size_t>(end);
    return {};
  }

  Result<void> parse_property(util::ByteReader& r, FbxProperty& prop) {
    prop.type = static_cast<char>(r.u8());
    switch (prop.type) {
      case 'Y': prop.integer = r.i16le(); break;
      case 'C': prop.integer = r.u8(); break;
      case 'I': prop.integer = r.i32le(); break;
      case 'L': prop.integer = r.i64le(); break;
      case 'F': prop.number = r.f32le(); break;
      case 'D': prop.number = r.f64le(); break;
      case 'S':
      case 'R': {
        const std::uint32_t length = r.u32le();
        prop.text = std::string(r.chars(length));
        break;
      }
      case 'f':
      case 'd':
      case 'l':
      case 'i':
      case 'b': {
        const std::uint32_t length = r.u32le();
        const std::uint32_t encoding = r.u32le();
        const std::uint32_t compressed = r.u32le();
        const std::size_t element = prop.type == 'f' || prop.type == 'i' ? 4 : prop.type == 'b' ? 1 : 8;
        const ByteView stored = r.bytes(compressed);
        if (!r.ok()) return fail(Status::Decode, "an FBX array is truncated");
        const std::uint64_t raw_size = std::uint64_t{length} * element;
        CVT_TRY(check_elements(ctx_, length, "FBX array values"));
        decompressed_ += raw_size;
        CVT_TRY(ctx_.limits.check_decompressed(decompressed_));
        Bytes inflated;
        ByteView raw = stored;
        if (encoding == 1) {
          CVT_TRY_ASSIGN(inflated, util::zlib_decompress(stored, raw_size));
          raw = inflated;
        } else if (encoding != 0) {
          return fail(Status::Decode, "unknown FBX array encoding {}", encoding);
        }
        if (raw.size() != raw_size) return fail(Status::Decode, "an FBX array has the wrong size");
        util::ByteReader a(raw);
        if (prop.type == 'f' || prop.type == 'd') {
          prop.numbers.resize(length);
          for (double& v : prop.numbers) v = prop.type == 'f' ? a.f32le() : a.f64le();
        } else {
          prop.integers.resize(length);
          for (std::int64_t& v : prop.integers) v = prop.type == 'i' ? a.i32le() : prop.type == 'l' ? a.i64le() : a.u8();
        }
        break;
      }
      default: return fail(Status::Decode, "unknown FBX property type 0x{:02x}", static_cast<unsigned>(prop.type));
    }
    if (!r.ok()) return fail(Status::Decode, "an FBX property is truncated");
    return {};
  }

  ByteView data_;
  const ReadContext& ctx_;
  std::uint32_t version_ = 0;
  std::size_t nodes_ = 0;
  std::uint64_t decompressed_ = 0;
};

// --------------------------------------------------------------------------- ASCII

class AsciiParser {
 public:
  AsciiParser(std::string_view text, const ReadContext& ctx) : text_(text), ctx_(ctx) {}

  Result<FbxNode> parse() {
    FbxNode root;
    CVT_TRY(parse_children(root, 0, false));
    return root;
  }

 private:
  enum class Tok { End, Name, Number, String, Comma, Open, Close, Star, Word };
  struct Token {
    Tok kind = Tok::End;
    std::string_view text;
  };

  void skip_space() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ';') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      } else if (util::is_ascii_space(c)) {
        ++pos_;
      } else {
        break;
      }
    }
  }

  Token peek() {
    const std::size_t save = pos_;
    const Token t = next();
    pos_ = save;
    return t;
  }

  Token next() {
    skip_space();
    if (pos_ >= text_.size()) return {};
    const char c = text_[pos_];
    if (c == ',') return {Tok::Comma, text_.substr(pos_++, 1)};
    if (c == '{') return {Tok::Open, text_.substr(pos_++, 1)};
    if (c == '}') return {Tok::Close, text_.substr(pos_++, 1)};
    if (c == '*') return {Tok::Star, text_.substr(pos_++, 1)};
    if (c == '"') {
      const std::size_t start = ++pos_;
      while (pos_ < text_.size() && text_[pos_] != '"') ++pos_;
      const std::string_view s = text_.substr(start, pos_ - start);
      if (pos_ < text_.size()) ++pos_;
      return {Tok::String, s};
    }
    const std::size_t start = pos_;
    while (pos_ < text_.size()) {
      const char d = text_[pos_];
      if (util::is_ascii_space(d) || d == ',' || d == '{' || d == '}' || d == ':' || d == '"' || d == ';') break;
      ++pos_;
    }
    const std::string_view word = text_.substr(start, pos_ - start);
    if (pos_ < text_.size() && text_[pos_] == ':') {
      ++pos_;
      return {Tok::Name, word};
    }
    if (word.empty()) {
      ++pos_;  // stray character
      return {Tok::Word, text_.substr(start, 1)};
    }
    const char f = word.front();
    if (util::is_ascii_digit(f) || f == '-' || f == '+' || f == '.') return {Tok::Number, word};
    return {Tok::Word, word};
  }

  static FbxProperty number_property(std::string_view text) {
    FbxProperty p;
    std::int64_t i = 0;
    if (parse_int(text, i)) {
      p.type = 'L';
      p.integer = i;
    } else {
      p.type = 'D';
      double d = 0;
      p.number = parse_double(text, d) ? d : 0.0;
    }
    return p;
  }

  Result<void> parse_children(FbxNode& parent, int depth, bool braced) {
    if (depth > max_depth) return fail(Status::Decode, "FBX nodes are nested too deeply");
    for (;;) {
      Token t = next();
      if (t.kind == Tok::End) {
        if (braced) return fail(Status::Decode, "the ASCII FBX file is truncated");
        return {};
      }
      if (t.kind == Tok::Close) {
        if (!braced) return fail(Status::Decode, "unbalanced '}' in the ASCII FBX file");
        return {};
      }
      if (t.kind != Tok::Name) return fail(Status::Decode, "unexpected '{}' in the ASCII FBX file", t.text);
      if (++nodes_ > 5'000'000) return fail(Status::Limit, "the FBX file has too many nodes");
      FbxNode node;
      node.name = std::string(t.text);
      // Properties: values separated by commas.
      Token p = peek();
      bool expect_value = p.kind == Tok::Number || p.kind == Tok::String || p.kind == Tok::Word || p.kind == Tok::Star;
      while (expect_value) {
        p = next();
        if (p.kind == Tok::Number) {
          node.props.push_back(number_property(p.text));
        } else if (p.kind == Tok::String) {
          FbxProperty s;
          s.type = 'S';
          s.text = std::string(p.text);
          node.props.push_back(std::move(s));
        } else if (p.kind == Tok::Word) {
          FbxProperty s;
          s.type = 'S';  // T/Y/W and other bare words
          s.text = std::string(p.text);
          node.props.push_back(std::move(s));
        } else if (p.kind == Tok::Star) {
          // *N { a: v, v, ... }
          const Token count = next();
          if (count.kind != Tok::Number || next().kind != Tok::Open) return fail(Status::Decode, "malformed ASCII FBX array");
          FbxProperty array;
          array.type = 'd';
          std::int64_t declared = 0;
          if (!parse_int(count.text, declared) || declared < 0) return fail(Status::Decode, "malformed ASCII FBX array");
          CVT_TRY(check_records(ctx_, static_cast<std::uint64_t>(declared), 1, text_.size() - pos_, "FBX array values"));
          Token a = next();
          if (a.kind == Tok::Name && a.text == "a") {
            bool all_int = true;
            std::vector<std::string_view> values;
            values.reserve(static_cast<std::size_t>(declared));
            for (;;) {
              const Token v = next();
              if (v.kind == Tok::Number) {
                values.push_back(v.text);
                if (v.text.find_first_of(".eE") != std::string_view::npos) all_int = false;
              } else if (v.kind == Tok::Close) {
                break;
              } else if (v.kind != Tok::Comma) {
                return fail(Status::Decode, "malformed ASCII FBX array");
              }
            }
            if (all_int) {
              array.type = 'l';
              for (const std::string_view v : values) {
                std::int64_t x = 0;
                array.integers.push_back(parse_int(v, x) ? x : 0);
              }
            } else {
              for (const std::string_view v : values) {
                double x = 0;
                array.numbers.push_back(parse_double(v, x) ? x : 0.0);
              }
            }
          } else if (a.kind != Tok::Close) {
            return fail(Status::Decode, "malformed ASCII FBX array");
          }
          node.props.push_back(std::move(array));
        }
        p = peek();
        if (p.kind != Tok::Comma) break;
        next();
        // FBX 6 style long arrays continue over lines after a comma.
        p = peek();
        expect_value = p.kind == Tok::Number || p.kind == Tok::String || p.kind == Tok::Word || p.kind == Tok::Star;
      }
      // Older files write plain number lists ("Vertices: 1,2,3"): fold them into an array.
      if (node.props.size() > 16 && std::all_of(node.props.begin(), node.props.end(), [](const FbxProperty& x) {
            return x.type == 'L' || x.type == 'D';
          })) {
        FbxProperty array;
        const bool ints = std::all_of(node.props.begin(), node.props.end(), [](const FbxProperty& x) { return x.type == 'L'; });
        array.type = ints ? 'l' : 'd';
        for (const FbxProperty& x : node.props) {
          if (ints) array.integers.push_back(x.integer);
          else array.numbers.push_back(x.as_double());
        }
        node.props.assign(1, std::move(array));
      }
      if (peek().kind == Tok::Open) {
        next();
        CVT_TRY(parse_children(node, depth + 1, true));
      }
      parent.children.push_back(std::move(node));
    }
  }

  std::string_view text_;
  const ReadContext& ctx_;
  std::size_t pos_ = 0;
  std::size_t nodes_ = 0;
};

// --------------------------------------------------------------------------- scene

std::vector<double> numbers_of(const FbxNode* node) {
  if (node == nullptr || node->props.empty()) return {};
  const FbxProperty& p = node->props[0];
  if (!p.numbers.empty()) return p.numbers;
  std::vector<double> out(p.integers.begin(), p.integers.end());
  if (out.empty() && !p.is_array()) {
    for (const FbxProperty& q : node->props) out.push_back(q.as_double());
  }
  return out;
}

std::vector<std::int64_t> ints_of(const FbxNode* node) {
  if (node == nullptr || node->props.empty()) return {};
  const FbxProperty& p = node->props[0];
  if (!p.integers.empty()) return p.integers;
  std::vector<std::int64_t> out;
  for (const double d : p.numbers) out.push_back(static_cast<std::int64_t>(d));
  if (out.empty() && !p.is_array()) {
    for (const FbxProperty& q : node->props) out.push_back(q.as_int());
  }
  return out;
}

std::string node_string(const FbxNode* node) {
  return node != nullptr && !node->props.empty() ? node->props[0].text : std::string();
}

// "Name\0\1Class" (binary) or "Class::Name" (ASCII) -> Name
std::string object_name(const FbxNode& node) {
  if (node.props.size() < 2) return {};
  const std::string& raw = node.props[1].text;
  if (const std::size_t sep = raw.find(std::string_view("\0\1", 2)); sep != std::string::npos) return raw.substr(0, sep);
  if (const std::size_t sep = raw.find("::"); sep != std::string::npos) return raw.substr(sep + 2);
  return raw;
}

std::int64_t object_id(const FbxNode& node) { return node.props.empty() ? 0 : node.props[0].as_int(); }

// Properties70 / Properties60 lookup: P: "Name", "type", "label", "flags", values...
struct PropertyTable {
  std::map<std::string, std::vector<double>, std::less<>> numbers;
  std::map<std::string, std::string, std::less<>> strings;

  explicit PropertyTable(const FbxNode* object) {
    if (object == nullptr) return;
    const FbxNode* table = object->child("Properties70");
    std::size_t first_value = 4;
    if (table == nullptr) {
      table = object->child("Properties60");
      first_value = 3;
    }
    if (table == nullptr) return;
    for (const FbxNode& p : table->children) {
      if ((p.name != "P" && p.name != "Property") || p.props.empty()) continue;
      std::vector<double> values;
      std::string text;
      for (std::size_t i = first_value; i < p.props.size(); ++i) {
        if (p.props[i].type == 'S') text = p.props[i].text;
        else values.push_back(p.props[i].as_double());
      }
      numbers[p.props[0].text] = std::move(values);
      strings[p.props[0].text] = std::move(text);
    }
  }
  [[nodiscard]] ir::Vec3 vec(std::string_view name, ir::Vec3 fallback) const {
    const auto it = numbers.find(name);
    if (it == numbers.end() || it->second.size() < 3) return fallback;
    return {static_cast<float>(it->second[0]), static_cast<float>(it->second[1]), static_cast<float>(it->second[2])};
  }
  [[nodiscard]] double number(std::string_view name, double fallback) const {
    const auto it = numbers.find(name);
    return it == numbers.end() || it->second.empty() ? fallback : it->second[0];
  }
  [[nodiscard]] bool has(std::string_view name) const { return numbers.contains(name); }
};

ir::Mat4 euler(const ir::Vec3& degrees, int order) {
  const ir::Mat4 x = rotation(degrees.x, 1, 0, 0), y = rotation(degrees.y, 0, 1, 0), z = rotation(degrees.z, 0, 0, 1);
  switch (order) {  // FBX eEuler orders: the first axis is applied first
    case 1: return y * z * x;  // XZY
    case 2: return x * z * y;  // YZX
    case 3: return z * x * y;  // YXZ
    case 4: return y * x * z;  // ZXY
    case 5: return x * y * z;  // ZYX
    default: return z * y * x;  // XYZ
  }
}

ir::Mat4 translate(const ir::Vec3& v) { return translation(v.x, v.y, v.z); }
ir::Mat4 inverse_translate(const ir::Vec3& v) { return translation(-v.x, -v.y, -v.z); }

struct ModelTransform {
  ir::Mat4 local;
  ir::Mat4 geometric;
};

ModelTransform model_transform(const FbxNode& model) {
  const PropertyTable p(&model);
  const int order = static_cast<int>(p.number("RotationOrder", 0));
  const ir::Vec3 t = p.vec("Lcl Translation", {0, 0, 0});
  const ir::Vec3 r = p.vec("Lcl Rotation", {0, 0, 0});
  const ir::Vec3 s = p.vec("Lcl Scaling", {1, 1, 1});
  const ir::Vec3 roff = p.vec("RotationOffset", {0, 0, 0});
  const ir::Vec3 rp = p.vec("RotationPivot", {0, 0, 0});
  const ir::Vec3 soff = p.vec("ScalingOffset", {0, 0, 0});
  const ir::Vec3 sp = p.vec("ScalingPivot", {0, 0, 0});
  const ir::Mat4 pre = euler(p.vec("PreRotation", {0, 0, 0}), 0);
  const ir::Mat4 post = euler(p.vec("PostRotation", {0, 0, 0}), 0);
  ModelTransform out;
  out.local = translate(t) * translate(roff) * translate(rp) * pre * euler(r, order) * inverse(post) * inverse_translate(rp) *
              translate(soff) * translate(sp) * scaling(s.x, s.y, s.z) * inverse_translate(sp);
  out.geometric = translate(p.vec("GeometricTranslation", {0, 0, 0})) * euler(p.vec("GeometricRotation", {0, 0, 0}), 0) *
                  [&] {
                    const ir::Vec3 g = p.vec("GeometricScaling", {1, 1, 1});
                    return scaling(g.x, g.y, g.z);
                  }();
  return out;
}

// Value index of a layer element for (polygon, corner, control point).
struct LayerElement {
  std::string mapping;
  std::string reference;
  std::vector<double> values;
  std::vector<std::int64_t> index;
  std::size_t width = 3;

  [[nodiscard]] std::int64_t value_index(std::size_t polygon, std::size_t corner, std::int64_t control) const {
    std::int64_t element = 0;
    if (mapping == "ByPolygonVertex") element = static_cast<std::int64_t>(corner);
    else if (mapping == "ByVertice" || mapping == "ByVertex" || mapping == "ByControlPoint") element = control;
    else if (mapping == "ByPolygon") element = static_cast<std::int64_t>(polygon);
    else if (mapping == "AllSame") element = 0;
    else return -1;
    if (reference == "IndexToDirect" || reference == "Index") {
      if (element < 0 || static_cast<std::size_t>(element) >= index.size()) return -1;
      element = index[static_cast<std::size_t>(element)];
    }
    if (element < 0 || static_cast<std::size_t>(element) * width + width > values.size()) return -1;
    return element;
  }
};

std::optional<LayerElement> layer(const FbxNode& geometry, std::string_view element_name, std::string_view values_name,
                                  std::string_view index_name, std::size_t width) {
  const FbxNode* e = geometry.child(element_name);
  if (e == nullptr) return std::nullopt;
  LayerElement out;
  out.mapping = node_string(e->child("MappingInformationType"));
  out.reference = node_string(e->child("ReferenceInformationType"));
  out.values = numbers_of(e->child(values_name));
  out.index = ints_of(e->child(index_name));
  out.width = width;
  if (out.values.empty()) return std::nullopt;
  return out;
}

struct CornerKey {
  std::int64_t v, n, t, c;
  friend bool operator==(const CornerKey&, const CornerKey&) = default;
};
struct CornerKeyHash {
  std::size_t operator()(const CornerKey& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::int64_t x : {k.v, k.n, k.t, k.c}) h = (h ^ static_cast<std::uint64_t>(x)) * 1099511628211ull;
    return static_cast<std::size_t>(h);
  }
};

struct Scene {
  std::unordered_map<std::int64_t, const FbxNode*> objects;
  std::unordered_multimap<std::int64_t, std::pair<std::int64_t, std::string>> children_of;  // parent -> (child, property)
  std::unordered_map<std::int64_t, std::int64_t> parent_model;
};

Result<ir::Mesh> geometry_mesh(const FbxNode& geometry, const ReadContext& ctx, std::vector<std::int64_t>& face_material_slot) {
  ir::Mesh part;
  const std::vector<double> vertices = numbers_of(geometry.child("Vertices"));
  const std::vector<std::int64_t> polygon_index = ints_of(geometry.child("PolygonVertexIndex"));
  const std::size_t control_points = vertices.size() / 3;
  CVT_TRY(check_elements(ctx, polygon_index.size(), "face corners"));
  const auto normals = layer(geometry, "LayerElementNormal", "Normals", "NormalsIndex", 3);
  auto uvs = layer(geometry, "LayerElementUV", "UV", "UVIndex", 2);
  const auto colors = layer(geometry, "LayerElementColor", "Colors", "ColorIndex", 4);
  std::optional<LayerElement> materials;
  if (const FbxNode* e = geometry.child("LayerElementMaterial")) {
    LayerElement m;
    m.mapping = node_string(e->child("MappingInformationType"));
    m.reference = "Direct";
    const std::vector<std::int64_t> slots = ints_of(e->child("Materials"));
    m.values.assign(slots.begin(), slots.end());
    m.width = 1;
    if (!m.values.empty()) materials = std::move(m);
  }
  std::unordered_map<CornerKey, std::uint32_t, CornerKeyHash> made;
  std::size_t polygon = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < polygon_index.size(); ++i) {
    if (polygon_index[i] >= 0) continue;
    const std::size_t corners = i + 1 - start;
    if (corners >= 3) {
      for (std::size_t k = start; k <= i; ++k) {
        std::int64_t v = polygon_index[k];
        if (v < 0) v = ~v;  // the last corner of a polygon is stored as -(index + 1)
        if (static_cast<std::size_t>(v) >= control_points) return fail(Status::Decode, "an FBX polygon refers to missing vertex {}", v);
        const CornerKey key{v, normals ? normals->value_index(polygon, k, v) : -1, uvs ? uvs->value_index(polygon, k, v) : -1,
                      colors ? colors->value_index(polygon, k, v) : -1};
        const auto [it, inserted] = made.emplace(key, static_cast<std::uint32_t>(part.positions.size()));
        if (inserted) {
          const auto at = static_cast<std::size_t>(v) * 3;
          part.positions.push_back({static_cast<float>(vertices[at]), static_cast<float>(vertices[at + 1]), static_cast<float>(vertices[at + 2])});
          if (normals) {
            const auto n = static_cast<std::size_t>(std::max<std::int64_t>(key.n, 0)) * 3;
            part.normals.push_back(key.n < 0 ? ir::Vec3{0, 0, 0}
                                              : ir::Vec3{static_cast<float>(normals->values[n]), static_cast<float>(normals->values[n + 1]),
                                                         static_cast<float>(normals->values[n + 2])});
          }
          if (uvs) {
            const auto t = static_cast<std::size_t>(std::max<std::int64_t>(key.t, 0)) * 2;
            part.texcoords.push_back(key.t < 0 ? ir::Vec2{} : ir::Vec2{static_cast<float>(uvs->values[t]), static_cast<float>(uvs->values[t + 1])});
          }
          if (colors) {
            const auto c = static_cast<std::size_t>(std::max<std::int64_t>(key.c, 0)) * 4;
            part.colors.push_back(key.c < 0 ? ir::Color{}
                                             : ir::Color{static_cast<float>(colors->values[c]), static_cast<float>(colors->values[c + 1]),
                                                         static_cast<float>(colors->values[c + 2]), static_cast<float>(colors->values[c + 3])});
          }
        }
        part.indices.push_back(it->second);
      }
      part.face_sizes.push_back(static_cast<std::uint32_t>(corners));
      const std::int64_t slot = materials ? materials->value_index(polygon, start, 0) : -1;
      face_material_slot.push_back(slot >= 0 ? static_cast<std::int64_t>(materials->values[static_cast<std::size_t>(slot)]) : 0);
    }
    ++polygon;
    start = i + 1;
  }
  return part;
}

Result<AnyIR> build_scene(const FbxNode& root, ReadContext& ctx) {
  const FbxNode* objects = root.child("Objects");
  if (objects == nullptr) return fail(Status::Decode, "the FBX file has no Objects section");
  Scene scene;
  for (const FbxNode& o : objects->children) scene.objects[object_id(o)] = &o;
  if (const FbxNode* connections = root.child("Connections")) {
    for (const FbxNode& c : connections->children) {
      if (c.props.size() < 3) continue;
      const std::string& kind = c.props[0].text;
      const std::int64_t child = c.props[1].as_int();
      const std::int64_t parent = c.props[2].as_int();
      const std::string property = c.props.size() > 3 ? c.props[3].text : std::string();
      scene.children_of.emplace(parent, std::make_pair(child, kind == "OP" ? property : std::string()));
      const auto co = scene.objects.find(child);
      const auto po = scene.objects.find(parent);
      if (co != scene.objects.end() && co->second->name == "Model" && (parent == 0 || (po != scene.objects.end() && po->second->name == "Model"))) {
        scene.parent_model[child] = parent;
      }
    }
  }
  ir::Mesh mesh;
  // Global settings: axes and unit.
  const PropertyTable settings(root.child("GlobalSettings"));
  const int up = static_cast<int>(settings.number("UpAxis", 1));
  const int up_sign = static_cast<int>(settings.number("UpAxisSign", 1));
  mesh.unit_meters = settings.number("UnitScaleFactor", 1.0) * 0.01;
  if (const FbxNode* header = root.child("FBXHeaderExtension")) {
    if (const FbxNode* info = header->child("SceneInfo")) {
      const PropertyTable p(info);
      for (const char* key : {"Original|ApplicationName", "LastSaved|ApplicationName"}) {
        if (const auto it = p.strings.find(key); it != p.strings.end() && !it->second.empty()) mesh.metadata["tool"] = it->second;
      }
      for (const char* key : {"Original|Author", "Author"}) {
        if (const auto it = p.strings.find(key); it != p.strings.end() && !it->second.empty()) mesh.metadata["author"] = it->second;
      }
    }
  }
  if (const FbxNode* creator = root.child("Creator"); creator != nullptr && mesh.metadata.empty()) {
    if (const std::string text = node_string(creator); !text.empty()) mesh.metadata["tool"] = text;
  }

  // World transform of a model (cycle-safe walk up the parents).
  const auto world_of = [&](std::int64_t id) -> Result<ir::Mat4> {
    std::vector<const FbxNode*> chain;
    std::int64_t current = id;
    for (int guard = 0; current != 0; ++guard) {
      if (guard > max_depth) return fail(Status::Decode, "the FBX model hierarchy is cyclic or too deep");
      const auto it = scene.objects.find(current);
      if (it == scene.objects.end() || it->second->name != "Model") break;
      chain.push_back(it->second);
      const auto parent = scene.parent_model.find(current);
      current = parent == scene.parent_model.end() ? 0 : parent->second;
    }
    ir::Mat4 world;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) world = world * model_transform(**it).local;
    return world;
  };

  std::map<std::int64_t, std::string> material_names;
  std::map<std::int64_t, int> texture_index;
  const auto material_name = [&](std::int64_t id) -> std::string {
    if (const auto it = material_names.find(id); it != material_names.end()) return it->second;
    const auto object = scene.objects.find(id);
    if (object == scene.objects.end() || object->second->name != "Material") return {};
    const FbxNode& node = *object->second;
    ir::Material m;
    m.name = object_name(node);
    if (m.name.empty()) m.name = std::format("material_{}", id);
    for (int k = 2; mesh.find_material(m.name) != nullptr; ++k) m.name = std::format("{}_{}", object_name(node), k);
    const PropertyTable p(&node);
    const ir::Vec3 diffuse = p.vec("DiffuseColor", p.vec("Diffuse", {0.8f, 0.8f, 0.8f}));
    const double factor = p.number("DiffuseFactor", 1.0);
    m.base_color = {static_cast<float>(diffuse.x * factor), static_cast<float>(diffuse.y * factor), static_cast<float>(diffuse.z * factor),
                    static_cast<float>(std::clamp(p.has("Opacity") ? p.number("Opacity", 1.0) : 1.0 - p.number("TransparencyFactor", 0.0), 0.0, 1.0))};
    const ir::Vec3 emissive = p.vec("EmissiveColor", {0, 0, 0});
    const double emissive_factor = p.number("EmissiveFactor", 1.0);
    m.emissive = {static_cast<float>(emissive.x * emissive_factor), static_cast<float>(emissive.y * emissive_factor),
                  static_cast<float>(emissive.z * emissive_factor)};
    const double shininess = p.number("Shininess", p.number("ShininessExponent", -1.0));
    if (shininess >= 0) m.roughness = static_cast<float>(std::clamp(std::sqrt(2.0 / (shininess + 2.0)), 0.0, 1.0));
    // Diffuse texture: Texture --OP "DiffuseColor"--> Material; Video --OO--> Texture.
    const auto range = scene.children_of.equal_range(id);
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second.second != "DiffuseColor" && it->second.second != "Diffuse") continue;
      const std::int64_t texture_id = it->second.first;
      if (const auto known = texture_index.find(texture_id); known != texture_index.end()) {
        m.base_color_texture = known->second;
        break;
      }
      const auto texture_object = scene.objects.find(texture_id);
      if (texture_object == scene.objects.end()) continue;
      ir::Texture texture;
      texture.uri = node_string(texture_object->second->child("RelativeFilename"));
      if (texture.uri.empty()) texture.uri = node_string(texture_object->second->child("FileName"));
      texture.name = std::string(base_name(texture.uri));
      const auto videos = scene.children_of.equal_range(texture_id);
      for (auto v = videos.first; v != videos.second; ++v) {
        const auto video = scene.objects.find(v->second.first);
        if (video == scene.objects.end() || video->second->name != "Video") continue;
        if (const FbxNode* content = video->second->child("Content"); content != nullptr && !content->props.empty()) {
          const std::string& bytes = content->props[0].text;
          texture.data = to_bytes(bytes);
          texture.mime = image_mime(texture.data, texture.uri);
        }
      }
      if (texture.data.empty()) ctx.warn(std::format("Texture '{}' is a separate file and was not included.", texture.name));
      m.base_color_texture = static_cast<int>(mesh.textures.size());
      texture_index[texture_id] = m.base_color_texture;
      mesh.textures.push_back(std::move(texture));
      break;
    }
    material_names[id] = m.name;
    mesh.materials.push_back(m);
    return m.name;
  };

  // Mesh models in file order.
  std::size_t done = 0;
  for (const FbxNode& model : objects->children) {
    if (model.name != "Model") continue;
    CVT_TRY(ctx.checkpoint(0.5 + 0.4 * static_cast<double>(++done) / (objects->children.size() + 1.0)));
    const std::int64_t id = object_id(model);
    const FbxNode* geometry = nullptr;
    std::vector<std::int64_t> material_ids;
    // Connections are stored in a multimap; restore file order for material slots.
    std::vector<std::int64_t> ordered;
    if (const FbxNode* connections = root.child("Connections")) {
      for (const FbxNode& c : connections->children) {
        if (c.props.size() >= 3 && c.props[2].as_int() == id) ordered.push_back(c.props[1].as_int());
      }
    }
    for (const std::int64_t child : ordered) {
      const auto object = scene.objects.find(child);
      if (object == scene.objects.end()) continue;
      if (object->second->name == "Geometry" && geometry == nullptr) geometry = object->second;
      if (object->second->name == "Material") material_ids.push_back(child);
    }
    if (geometry == nullptr || !geometry->child("Vertices")) continue;
    std::vector<std::int64_t> slots;
    CVT_TRY_ASSIGN(ir::Mesh part, geometry_mesh(*geometry, ctx, slots));
    if (part.positions.empty()) continue;
    CVT_TRY_ASSIGN(const ir::Mat4 world, world_of(id));
    transform(part, world * model_transform(model).geometric);
    const std::string name = object_name(model);
    for (std::size_t f = 0; f < slots.size(); ++f) {
      const std::int64_t slot = slots[f];
      std::string material;
      if (slot >= 0 && static_cast<std::size_t>(slot) < material_ids.size()) material = material_name(material_ids[slot]);
      if (part.groups.empty() || part.groups.back().material != material) part.groups.push_back({name, material, f, 0});
      part.groups.back().face_count++;
    }
    const std::size_t first = mesh.face_count();
    const auto materials = std::move(mesh.materials);
    const auto textures = std::move(mesh.textures);
    append(mesh, part);
    mesh.materials = materials;
    mesh.textures = textures;
    mesh.objects.push_back({name, first, mesh.face_count() - first, world});
    CVT_TRY(check_elements(ctx, mesh.positions.size(), "vertices"));
  }
  if (mesh.positions.empty()) return fail(Status::Decode, "the FBX file contains no mesh geometry");
  if (mesh.objects.size() == 1 && mesh.objects[0].transform.is_identity()) mesh.objects.clear();
  if (up == 2 && up_sign > 0) {
    mesh.source_up_axis = "Z";
    convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  } else if (up == 1 && up_sign > 0) {
    mesh.source_up_axis = "Y";
  } else {
    ctx.warn("The FBX file uses an unusual up axis; the model was not reoriented.");
  }
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

Result<AnyIR> read_fbx(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  if (text.starts_with(std::string_view("Kaydara FBX Binary  \0", 21))) {
    BinaryParser parser(ctx.input, ctx);
    CVT_TRY_ASSIGN(const FbxNode root, parser.parse());
    CVT_TRY(ctx.checkpoint(0.4, "Parsing"));
    return build_scene(root, ctx);
  }
  if (!looks_textual(ctx.input)) return fail(Status::Decode, "not an FBX file");
  AsciiParser parser(text, ctx);
  CVT_TRY_ASSIGN(const FbxNode root, parser.parse());
  CVT_TRY(ctx.checkpoint(0.4, "Parsing"));
  if (root.child("Objects") == nullptr) return fail(Status::Decode, "not an FBX file");
  return build_scene(root, ctx);
}

// --------------------------------------------------------------------------- writing (ASCII 7.4)

constexpr WriterTraits fbx_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.objects = true;
  traits.up = WriterTraits::Up::Y;
  return traits;
}

std::string fbx_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '"') out += "&quot;";
    else if (c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  return out;
}

// Splits a matrix into translation, XYZ Euler rotation (degrees) and
// scale; false when it has shear or projection.
bool decompose(const ir::Mat4& m, ir::Vec3& t, ir::Vec3& r, ir::Vec3& s) {
  t = {m.m[12], m.m[13], m.m[14]};
  const ir::Vec3 c0{m.m[0], m.m[1], m.m[2]}, c1{m.m[4], m.m[5], m.m[6]}, c2{m.m[8], m.m[9], m.m[10]};
  s = {length(c0), length(c1), length(c2)};
  if (!(s.x > 0 && s.y > 0 && s.z > 0)) return false;
  if (determinant3(m) < 0) s.x = -s.x;
  const ir::Vec3 x = mul(c0, 1 / s.x), y = mul(c1, 1 / s.y), z = mul(c2, 1 / s.z);
  // R = Rz * Ry * Rx; R[2][0] = -sin(ry)
  const double sy = std::clamp(-static_cast<double>(x.z), -1.0, 1.0);
  const double ry = std::asin(sy);
  double rx = 0, rz = 0;
  if (std::abs(sy) < 0.99999) {
    rx = std::atan2(y.z, z.z);
    rz = std::atan2(x.y, x.x);
  } else {
    rx = std::atan2(-z.y, y.y);
  }
  const double k = 180.0 / std::numbers::pi;
  r = {static_cast<float>(rx * k), static_cast<float>(ry * k), static_cast<float>(rz * k)};
  const ir::Mat4 rebuilt = translation(t.x, t.y, t.z) * euler(r, 0) * scaling(s.x, s.y, s.z);
  for (int i = 0; i < 16; ++i) {
    if (std::abs(rebuilt.m[i] - m.m[i]) > 1e-4f * (1.0f + std::abs(m.m[i]))) return false;
  }
  return true;
}

void fbx_array(std::string& out, std::string_view indent, std::string_view name, const std::vector<double>& values, int precision) {
  out += std::format("{}{}: *{} {{\n{}\ta: ", indent, name, values.size(), indent);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    append_number(out, values[i], precision);
  }
  out += std::format("\n{}}}\n", indent);
}

void fbx_int_array(std::string& out, std::string_view indent, std::string_view name, const std::vector<std::int64_t>& values) {
  out += std::format("{}{}: *{} {{\n{}\ta: ", indent, name, values.size(), indent);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    out += std::to_string(values[i]);
  }
  out += std::format("\n{}}}\n", indent);
}

std::string fbx_vec(const ir::Vec3& v) {
  std::string out;
  append_number(out, v.x);
  out += ',';
  append_number(out, v.y);
  out += ',';
  append_number(out, v.z);
  return out;
}

Result<Bytes> write_fbx(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, fbx_traits()));
  // Unitless models are declared in metres (UnitScaleFactor 100), the unit
  // most FBX importers assume for scene-scale content.
  const UnitChoice unit = resolve_unit(mesh, ctx, 1.0);
  if (unit.scale != 1.0f) {
    scale(mesh, unit.scale);
    for (ir::MeshObject& o : mesh.objects) {
      for (int r = 0; r < 3; ++r) o.transform.m[12 + r] *= unit.scale;
    }
  }
  const int precision = write_precision(ctx);
  const bool strip = options::resolved_strip_metadata(ctx.options);
  std::vector<ObjectPart> parts = split_objects(mesh, true);
  std::string out;
  out += "; FBX 7.4.0 project file\n; ----------------------------------------------------\n\n";
  out += "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreationTimeStamp:  {\n\t\tVersion: 1000\n"
         "\t\tYear: 1970\n\t\tMonth: 1\n\t\tDay: 1\n\t\tHour: 0\n\t\tMinute: 0\n\t\tSecond: 0\n\t\tMillisecond: 0\n\t}\n";
  out += std::format("\tCreator: \"{}\"\n}}\n", strip ? "" : "Convertible");
  out += "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
         "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "\t\tP: \"OriginalUpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"OriginalUpAxisSign\", \"int\", \"Integer\", \"\",1\n";
  std::string factor;
  append_number(factor, unit.meters * 100.0);
  out += std::format("\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",{0}\n"
                     "\t\tP: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\",{0}\n\t}}\n}}\n",
                     factor);
  out += "Documents:  {\n\tCount: 1\n\tDocument: 1000000000, \"\", \"Scene\" {\n\t\tRootNode: 0\n\t}\n}\nReferences:  {\n}\n";
  const std::size_t models = parts.size();
  out += std::format("Definitions:  {{\n\tVersion: 100\n\tCount: {}\n\tObjectType: \"GlobalSettings\" {{\n\t\tCount: 1\n\t}}\n"
                     "\tObjectType: \"Model\" {{\n\t\tCount: {}\n\t}}\n\tObjectType: \"Geometry\" {{\n\t\tCount: {}\n\t}}\n",
                     1 + 2 * models + mesh.materials.size(), models, models);
  if (!mesh.materials.empty()) out += std::format("\tObjectType: \"Material\" {{\n\t\tCount: {}\n\t}}\n", mesh.materials.size());
  out += "}\nObjects:  {\n";
  std::string connections = "Connections:  {\n";
  std::map<std::string, std::size_t> material_index;
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) material_index.emplace(mesh.materials[i].name, i);
  for (std::size_t p = 0; p < parts.size(); ++p) {
    CVT_TRY(ctx.checkpoint(0.8 + 0.15 * static_cast<double>(p) / parts.size()));
    ir::Mesh& part = parts[p].mesh;
    ir::Vec3 t{0, 0, 0}, r{0, 0, 0}, s{1, 1, 1};
    if (!parts[p].transform.is_identity() && !decompose(parts[p].transform, t, r, s)) {
      transform(part, parts[p].transform);  // shear: bake the transform into the geometry
      t = {0, 0, 0};
      r = {0, 0, 0};
      s = {1, 1, 1};
    }
    const std::int64_t geometry_id = 2000000 + static_cast<std::int64_t>(p);
    const std::int64_t model_id = 3000000 + static_cast<std::int64_t>(p);
    const std::string name = fbx_escape(parts[p].name.empty() ? std::format("Object{}", p) : parts[p].name);
    out += std::format("\tGeometry: {}, \"Geometry::{}\", \"Mesh\" {{\n", geometry_id, name);
    std::vector<double> values;
    for (const ir::Vec3& v : part.positions) values.insert(values.end(), {v.x, v.y, v.z});
    fbx_array(out, "\t\t", "Vertices", values, precision);
    std::vector<std::int64_t> polygon_index;
    const std::vector<std::size_t> offsets = face_offsets(part);
    for (std::size_t f = 0; f + 1 < offsets.size(); ++f) {
      for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) {
        const auto index = static_cast<std::int64_t>(part.indices[c]);
        polygon_index.push_back(c + 1 == offsets[f + 1] ? ~index : index);
      }
    }
    fbx_int_array(out, "\t\t", "PolygonVertexIndex", polygon_index);
    out += "\t\tGeometryVersion: 124\n";
    std::string layer = "\t\tLayer: 0 {\n\t\t\tVersion: 100\n";
    if (!part.normals.empty()) {
      out += "\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByVertice\"\n"
             "\t\t\tReferenceInformationType: \"Direct\"\n";
      values.clear();
      for (const ir::Vec3& v : part.normals) values.insert(values.end(), {v.x, v.y, v.z});
      fbx_array(out, "\t\t\t", "Normals", values, precision == 0 ? 7 : precision);
      out += "\t\t}\n";
      layer += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
    }
    if (!part.texcoords.empty()) {
      out += "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVMap\"\n\t\t\tMappingInformationType: \"ByVertice\"\n"
             "\t\t\tReferenceInformationType: \"Direct\"\n";
      values.clear();
      for (const ir::Vec2& v : part.texcoords) values.insert(values.end(), {v.x, v.y});
      fbx_array(out, "\t\t\t", "UV", values, precision);
      out += "\t\t}\n";
      layer += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
    }
    if (!part.colors.empty()) {
      out += "\t\tLayerElementColor: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"Col\"\n\t\t\tMappingInformationType: \"ByVertice\"\n"
             "\t\t\tReferenceInformationType: \"Direct\"\n";
      values.clear();
      for (const ir::Color& c : part.colors) values.insert(values.end(), {c.r, c.g, c.b, c.a});
      fbx_array(out, "\t\t\t", "Colors", values, precision == 0 ? 7 : precision);
      out += "\t\t}\n";
      layer += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementColor\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
    }
    // Materials used by this part, in slot order.
    std::vector<std::size_t> slot_material;
    std::vector<std::int64_t> face_slot(part.face_count(), 0);
    for (const ir::MeshGroup& g : part.groups) {
      const auto it = material_index.find(g.material);
      if (it == material_index.end()) continue;
      auto slot = std::find(slot_material.begin(), slot_material.end(), it->second);
      if (slot == slot_material.end()) {
        slot_material.push_back(it->second);
        slot = slot_material.end() - 1;
      }
      for (std::size_t f = g.first_face; f < g.first_face + g.face_count && f < face_slot.size(); ++f) {
        face_slot[f] = slot - slot_material.begin();
      }
    }
    if (!slot_material.empty()) {
      out += "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygon\"\n"
             "\t\t\tReferenceInformationType: \"IndexToDirect\"\n";
      fbx_int_array(out, "\t\t\t", "Materials", face_slot);
      out += "\t\t}\n";
      layer += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
    }
    out += layer + "\t\t}\n\t}\n";
    out += std::format("\tModel: {}, \"Model::{}\", \"Mesh\" {{\n\t\tVersion: 232\n\t\tProperties70:  {{\n", model_id, name);
    out += std::format("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",{}\n", fbx_vec(t));
    out += std::format("\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",{}\n", fbx_vec(r));
    out += std::format("\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",{}\n", fbx_vec(s));
    out += "\t\t}\n\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n";
    connections += std::format("\tC: \"OO\",{},0\n\tC: \"OO\",{},{}\n", model_id, geometry_id, model_id);
    for (const std::size_t m : slot_material) connections += std::format("\tC: \"OO\",{},{}\n", 4000000 + m, model_id);
  }
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
    const ir::Material& m = mesh.materials[i];
    out += std::format("\tMaterial: {}, \"Material::{}\", \"\" {{\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t\tMultiLayer: 0\n"
                       "\t\tProperties70:  {{\n",
                       4000000 + i, fbx_escape(m.name));
    out += std::format("\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",{}\n", fbx_vec({m.base_color.r, m.base_color.g, m.base_color.b}));
    out += std::format("\t\t\tP: \"EmissiveColor\", \"Color\", \"\", \"A\",{}\n", fbx_vec(m.emissive));
    std::string opacity, shininess;
    append_number(opacity, m.base_color.a, 6);
    const float rough = std::clamp(m.roughness, 0.02f, 1.0f);
    append_number(shininess, std::clamp(2.0f / (rough * rough) - 2.0f, 0.0f, 1000.0f), 6);
    out += std::format("\t\t\tP: \"Opacity\", \"double\", \"Number\", \"\",{}\n\t\t\tP: \"Shininess\", \"double\", \"Number\", \"\",{}\n"
                       "\t\t}}\n\t}}\n",
                       opacity, shininess);
  }
  if (!mesh.textures.empty()) ctx.warn("FBX output does not embed textures; material colours were kept.");
  out += "}\n" + connections + "}\n";
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

int sniff_fbx(ByteView data, std::string_view /*filename*/) {
  const std::string_view head = text_head(data, 2048);
  if (head.starts_with(std::string_view("Kaydara FBX Binary  \0", 21))) return 100;
  if (head.starts_with("; FBX") && head.find("FBXHeaderExtension") != std::string_view::npos) return 95;
  if (head.find("FBXHeaderExtension:") != std::string_view::npos) return 85;
  return 0;
}

}  // namespace

void register_fbx(Registry& registry) {
  FormatInfo info;
  info.id = "fbx";
  info.name = "Autodesk FBX model";
  info.description = "Widely used scene exchange format (binary and ASCII).";
  info.category = Category::Model3D;
  info.extensions = {"fbx"};
  info.mime = "application/octet-stream";
  info.popularity = 50;
  info.sniff = sniff_fbx;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "fbx";
  codec.kind = Kind::Mesh;
  codec.read = read_fbx;
  codec.write = write_fbx;
  codec.write_options = mesh_write_options(fbx_traits());
  codec.write_options.push_back(unit_option());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::mesh
