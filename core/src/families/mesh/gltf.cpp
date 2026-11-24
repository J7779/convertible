// Convertible core - mesh family: glTF 2.0 (.gltf JSON and .glb binary).
//
// Reader: the default scene's node hierarchy is flattened into scene space
// (one ir::MeshObject per node with a mesh), every accessor layout of the
// specification is decoded (all component types, normalized integers,
// byteStride, sparse substitution, accessors without a buffer view),
// triangle strips and fans become triangle lists, and metallic-roughness
// materials with their embedded images are kept.
//
// Writer: indexed triangles, normals, TEXCOORD_0, COLOR_0, PBR materials
// and embedded PNG/JPEG textures, one node per object with its matrix, and
// optional KHR_mesh_quantization. The output follows the validator's rules:
// POSITION min/max, aligned offsets, exact buffer lengths, unit normals.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/base64.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

constexpr std::uint32_t glb_magic = 0x46546C67;  // "glTF"
constexpr std::uint32_t chunk_json = 0x4E4F534A;
constexpr std::uint32_t chunk_bin = 0x004E4942;

constexpr int ct_byte = 5120, ct_ubyte = 5121, ct_short = 5122, ct_ushort = 5123, ct_uint = 5125, ct_float = 5126;

std::size_t component_size(std::int64_t type) {
  switch (type) {
    case ct_byte:
    case ct_ubyte: return 1;
    case ct_short:
    case ct_ushort: return 2;
    case ct_uint:
    case ct_float: return 4;
    default: return 0;
  }
}

std::size_t component_count(std::string_view type) {
  if (type == "SCALAR") return 1;
  if (type == "VEC2") return 2;
  if (type == "VEC3") return 3;
  if (type == "VEC4" || type == "MAT2") return 4;
  if (type == "MAT3") return 9;
  if (type == "MAT4") return 16;
  return 0;
}

// Index of an array element referenced by `key` in `object`, or -1.
std::int64_t ref(const Json& object, std::string_view key) {
  const Json* value = object.find(key);
  if (value == nullptr || !value->is_integer()) return -1;
  return value->as_int64(-1);
}

float number(const Json& value, float fallback) {
  return value.is_number() ? static_cast<float>(value.as_double(fallback)) : fallback;
}

// Decoded glTF document with resolved buffers.
class GltfDocument {
 public:
  GltfDocument(const Json& root, std::vector<std::optional<ByteView>> buffers, ReadContext& ctx)
      : root_(root), buffers_(std::move(buffers)), ctx_(ctx) {}

  // Reads an accessor as floats (normalized integers mapped to [0,1] or
  // [-1,1] when `normalize_ints` or the accessor says so).
  Result<std::vector<float>> floats(std::int64_t index, std::size_t& components) const {
    CVT_TRY_ASSIGN(const Raw raw, layout(index));
    components = raw.components;
    std::vector<float> out(raw.count * raw.components, 0.0f);
    if (raw.data != nullptr) {
      for (std::size_t i = 0; i < raw.count; ++i) {
        for (std::size_t c = 0; c < raw.components; ++c) {
          out[i * raw.components + c] = read_component(raw, raw.data + i * raw.stride + c * raw.size);
        }
      }
    }
    CVT_TRY(apply_sparse(index, raw, [&](std::size_t element, std::size_t c, float v) {
      out[element * raw.components + c] = v;
    }));
    return out;
  }

  Result<std::vector<std::uint32_t>> indices(std::int64_t index) const {
    CVT_TRY_ASSIGN(const Raw raw, layout(index));
    if (raw.components != 1 || raw.type == ct_float || raw.type == ct_byte || raw.type == ct_short) {
      return fail(Status::Decode, "glTF index accessor {} has an invalid type", index);
    }
    std::vector<std::uint32_t> out(raw.count, 0);
    if (raw.data != nullptr) {
      for (std::size_t i = 0; i < raw.count; ++i) out[i] = read_uint(raw, raw.data + i * raw.stride);
    }
    CVT_TRY(apply_sparse(index, raw, [&](std::size_t element, std::size_t, float v) {
      out[element] = static_cast<std::uint32_t>(v);
    }));
    return out;
  }

  // The bytes of a buffer view, or nullopt.
  [[nodiscard]] std::optional<ByteView> view_bytes(std::int64_t view) const {
    const Json& views = root_["bufferViews"];
    if (view < 0 || static_cast<std::size_t>(view) >= views.size()) return std::nullopt;
    const Json& v = views[static_cast<std::size_t>(view)];
    const std::int64_t buffer = ref(v, "buffer");
    if (buffer < 0 || static_cast<std::size_t>(buffer) >= buffers_.size() || !buffers_[buffer]) return std::nullopt;
    const ByteView data = *buffers_[buffer];
    const std::uint64_t offset = v["byteOffset"].as_uint64(0);
    const std::uint64_t length = v["byteLength"].as_uint64(0);
    if (offset > data.size() || length > data.size() - offset) return std::nullopt;
    return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
  }

 private:
  struct Raw {
    const std::byte* data = nullptr;  // null: no buffer view (all zeros)
    std::size_t count = 0;
    std::size_t components = 0;
    std::size_t size = 0;
    std::size_t stride = 0;
    std::int64_t type = 0;
    bool normalized = false;
  };

  Result<Raw> layout(std::int64_t index) const {
    const Json& accessors = root_["accessors"];
    if (index < 0 || static_cast<std::size_t>(index) >= accessors.size()) {
      return fail(Status::Decode, "glTF accessor {} does not exist", index);
    }
    const Json& a = accessors[static_cast<std::size_t>(index)];
    Raw raw;
    raw.type = a["componentType"].as_int64(0);
    raw.size = component_size(raw.type);
    raw.components = component_count(a["type"].as_string());
    raw.normalized = a["normalized"].as_bool(false);
    const std::uint64_t count = a["count"].as_uint64(0);
    if (raw.size == 0 || raw.components == 0) return fail(Status::Decode, "glTF accessor {} has an invalid type", index);
    CVT_TRY(check_elements(ctx_, count * raw.components, "accessor values"));
    raw.count = static_cast<std::size_t>(count);
    const std::int64_t view_index = ref(a, "bufferView");
    if (view_index < 0) return raw;  // zeros (sparse may fill it)
    const std::optional<ByteView> view = view_bytes(view_index);
    if (!view) {
      return fail(Status::Decode, "glTF buffer view {} is missing or lies outside its buffer (external .bin files "
                  "cannot be read; use a .glb or a .gltf with embedded buffers)", view_index);
    }
    const Json& v = root_["bufferViews"][static_cast<std::size_t>(view_index)];
    const std::uint64_t element = raw.size * raw.components;
    const std::uint64_t stride = v["byteStride"].as_uint64(0);
    raw.stride = static_cast<std::size_t>(stride != 0 ? stride : element);
    if (stride != 0 && stride < element) return fail(Status::Decode, "glTF byteStride is smaller than an element");
    const std::uint64_t offset = a["byteOffset"].as_uint64(0);
    if (raw.count > 0) {
      const std::uint64_t needed = offset + (count - 1) * raw.stride + element;
      if (offset > view->size() || needed > view->size()) {
        return fail(Status::Decode, "glTF accessor {} reads past the end of its buffer view", index);
      }
    }
    raw.data = view->data() + offset;
    return raw;
  }

  static float read_component(const Raw& raw, const std::byte* p) {
    util::ByteReader r(ByteView(p, raw.size));
    switch (raw.type) {
      case ct_float: return r.f32le();
      case ct_byte: {
        const double v = r.i8();
        return static_cast<float>(raw.normalized ? std::max(v / 127.0, -1.0) : v);
      }
      case ct_ubyte: {
        const double v = r.u8();
        return static_cast<float>(raw.normalized ? v / 255.0 : v);
      }
      case ct_short: {
        const double v = r.i16le();
        return static_cast<float>(raw.normalized ? std::max(v / 32767.0, -1.0) : v);
      }
      case ct_ushort: {
        const double v = r.u16le();
        return static_cast<float>(raw.normalized ? v / 65535.0 : v);
      }
      case ct_uint: return static_cast<float>(r.u32le());
      default: return 0.0f;
    }
  }
  static std::uint32_t read_uint(const Raw& raw, const std::byte* p) {
    util::ByteReader r(ByteView(p, raw.size));
    switch (raw.type) {
      case ct_ubyte: return r.u8();
      case ct_ushort: return r.u16le();
      default: return r.u32le();
    }
  }

  template <class Fn>
  Result<void> apply_sparse(std::int64_t index, const Raw& raw, Fn&& set) const {
    const Json& sparse = root_["accessors"][static_cast<std::size_t>(index)]["sparse"];
    if (!sparse.is_object()) return {};
    const std::uint64_t count = sparse["count"].as_uint64(0);
    if (count == 0) return {};
    if (count > raw.count) return fail(Status::Decode, "glTF sparse accessor {} is larger than its accessor", index);
    const Json& idx = sparse["indices"];
    const Json& val = sparse["values"];
    const std::optional<ByteView> iview = view_bytes(ref(idx, "bufferView"));
    const std::optional<ByteView> vview = view_bytes(ref(val, "bufferView"));
    if (!iview || !vview) return fail(Status::Decode, "glTF sparse accessor {} has missing data", index);
    Raw ir;
    ir.type = idx["componentType"].as_int64(0);
    ir.size = component_size(ir.type);
    ir.components = 1;
    ir.stride = ir.size;
    if (ir.size == 0 || ir.type == ct_float) return fail(Status::Decode, "glTF sparse indices have an invalid type");
    const std::uint64_t ioff = idx["byteOffset"].as_uint64(0);
    const std::uint64_t voff = val["byteOffset"].as_uint64(0);
    const std::uint64_t element = raw.size * raw.components;
    if (ioff > iview->size() || count * ir.size > iview->size() - ioff || voff > vview->size() ||
        count * element > vview->size() - voff) {
      return fail(Status::Decode, "glTF sparse accessor {} reads past its buffer view", index);
    }
    Raw vr = raw;
    vr.stride = static_cast<std::size_t>(element);
    for (std::uint64_t k = 0; k < count; ++k) {
      const std::uint32_t target = read_uint(ir, iview->data() + ioff + k * ir.size);
      if (target >= raw.count) return fail(Status::Decode, "glTF sparse index {} is out of range", target);
      for (std::size_t c = 0; c < raw.components; ++c) {
        set(target, c, read_component(vr, vview->data() + voff + k * element + c * raw.size));
      }
    }
    return {};
  }

  const Json& root_;
  std::vector<std::optional<ByteView>> buffers_;
  ReadContext& ctx_;
};

std::optional<Bytes> decode_data_uri(std::string_view uri, std::string* mime) {
  if (!uri.starts_with("data:")) return std::nullopt;
  const std::size_t comma = uri.find(',');
  if (comma == std::string_view::npos) return std::nullopt;
  const std::string_view meta = uri.substr(5, comma - 5);
  if (!meta.ends_with(";base64")) return std::nullopt;
  if (mime != nullptr) *mime = std::string(meta.substr(0, meta.size() - 7));
  auto decoded = util::base64_decode(uri.substr(comma + 1));
  if (!decoded) return std::nullopt;
  return std::move(*decoded);
}

ir::Mat4 node_matrix(const Json& node) {
  const Json& m = node["matrix"];
  if (m.is_array() && m.size() == 16) {
    ir::Mat4 out;
    for (std::size_t i = 0; i < 16; ++i) out.m[i] = number(m[i], out.m[i]);
    return out;
  }
  ir::Mat4 t;
  const Json& tr = node["translation"];
  if (tr.is_array() && tr.size() == 3) t = translation(number(tr[0], 0), number(tr[1], 0), number(tr[2], 0));
  ir::Mat4 r;
  const Json& q = node["rotation"];
  if (q.is_array() && q.size() == 4) r = rotation_quaternion(number(q[0], 0), number(q[1], 0), number(q[2], 0), number(q[3], 1));
  ir::Mat4 s;
  const Json& sc = node["scale"];
  if (sc.is_array() && sc.size() == 3) s = scaling(number(sc[0], 1), number(sc[1], 1), number(sc[2], 1));
  return t * r * s;
}

Result<AnyIR> read_gltf(ReadContext& ctx) {
  ByteView input = ctx.input;
  std::string_view json_text;
  std::optional<ByteView> bin;
  util::ByteReader r(input);
  if (r.u32le() == glb_magic && input.size() >= 12) {
    const std::uint32_t version = r.u32le();
    const std::uint32_t length = r.u32le();
    if (version != 2) return fail(Status::Decode, "unsupported glTF binary version {}", version);
    if (length > input.size()) return fail(Status::Decode, "the GLB file is truncated");
    if (length < input.size()) input = input.first(length);
    r = util::ByteReader(input);
    r.skip(12);
    while (r.remaining() >= 8) {
      const std::uint32_t chunk_length = r.u32le();
      const std::uint32_t type = r.u32le();
      const ByteView chunk = r.bytes(chunk_length);
      if (!r.ok()) return fail(Status::Decode, "a GLB chunk is truncated");
      if (type == chunk_json && json_text.empty()) json_text = as_chars(chunk);
      else if (type == chunk_bin && !bin) bin = chunk;
    }
    if (json_text.empty()) return fail(Status::Decode, "the GLB file has no JSON chunk");
  } else {
    json_text = as_chars(ctx.input);
  }
  CVT_TRY_ASSIGN(const Json root, util::parse_json(json_text));
  if (!root.is_object() || !root["asset"].is_object()) return fail(Status::Decode, "not a glTF document");
  const std::string version = root["asset"]["version"].as_string();
  if (!version.starts_with("2.")) return fail(Status::Decode, "glTF version '{}' is not supported (only 2.x)", version);
  for (const Json& required : root["extensionsRequired"].elements()) {
    const std::string& name = required.as_string();
    if (name == "KHR_draco_mesh_compression" || name == "EXT_meshopt_compression") {
      return fail(Status::Decode, "the glTF file uses {} compressed geometry, which is not supported", name);
    }
    if (name != "KHR_mesh_quantization" && name != "KHR_texture_transform" && name != "KHR_materials_unlit") {
      ctx.warn(std::format("The glTF extension {} is not supported and was ignored.", name));
    }
  }

  // Buffers.
  std::vector<std::optional<ByteView>> buffers;
  std::vector<Bytes> decoded;  // owns data-URI buffers
  decoded.reserve(root["buffers"].size());
  for (std::size_t i = 0; i < root["buffers"].size(); ++i) {
    const Json& b = root["buffers"][i];
    const Json* uri = b.find("uri");
    if (uri == nullptr) {
      buffers.push_back(i == 0 && bin ? bin : std::nullopt);
      continue;
    }
    std::optional<Bytes> data = decode_data_uri(uri->as_string(), nullptr);
    if (!data) {
      ctx.warn(std::format("The glTF buffer '{}' is a separate file and could not be read.", uri->as_string()));
      buffers.push_back(std::nullopt);
      continue;
    }
    CVT_TRY(ctx.limits.check_decompressed(data->size()));
    decoded.push_back(std::move(*data));
    buffers.push_back(ByteView(decoded.back()));
  }
  const GltfDocument doc(root, buffers, ctx);

  ir::Mesh mesh;
  mesh.unit_meters = 1.0;
  mesh.source_up_axis = "Y";
  if (const std::string& generator = root["asset"]["generator"].as_string(); !generator.empty()) {
    mesh.metadata["tool"] = generator;
  }
  if (const std::string& copyright = root["asset"]["copyright"].as_string(); !copyright.empty()) {
    mesh.metadata["copyright"] = copyright;
  }
  // Images -> textures (same indices).
  for (std::size_t i = 0; i < root["images"].size(); ++i) {
    const Json& image = root["images"][i];
    ir::Texture texture;
    texture.name = image["name"].as_string();
    texture.mime = image["mimeType"].as_string();
    if (const std::int64_t view = ref(image, "bufferView"); view >= 0) {
      if (const auto bytes = doc.view_bytes(view)) texture.data = to_bytes(*bytes);
    } else if (const std::string& uri = image["uri"].as_string(); !uri.empty()) {
      std::string mime;
      if (auto data = decode_data_uri(uri, &mime)) {
        texture.data = std::move(*data);
        if (texture.mime.empty()) texture.mime = mime;
      } else {
        texture.uri = uri;
        ctx.warn(std::format("The glTF image '{}' is a separate file and was not included.", uri));
      }
    }
    if (texture.mime.empty() && !texture.data.empty()) texture.mime = image_mime(texture.data, texture.uri);
    if (texture.name.empty()) texture.name = texture.uri.empty() ? std::format("image_{}", i) : std::string(base_name(texture.uri));
    mesh.textures.push_back(std::move(texture));
  }
  const auto texture_image = [&](const Json& info) -> int {
    const std::int64_t t = ref(info, "index");
    if (t < 0 || static_cast<std::size_t>(t) >= root["textures"].size()) return -1;
    const std::int64_t source = ref(root["textures"][static_cast<std::size_t>(t)], "source");
    return source >= 0 && static_cast<std::size_t>(source) < mesh.textures.size() ? static_cast<int>(source) : -1;
  };
  std::vector<std::string> material_names;
  for (std::size_t i = 0; i < root["materials"].size(); ++i) {
    const Json& m = root["materials"][i];
    ir::Material material;
    material.name = m["name"].as_string();
    if (material.name.empty() || mesh.find_material(material.name) != nullptr) {
      material.name = std::format("{}{}material_{}", material.name, material.name.empty() ? "" : "_", i);
    }
    const Json& pbr = m["pbrMetallicRoughness"];
    const Json& factor = pbr["baseColorFactor"];
    if (factor.is_array() && factor.size() == 4) {
      material.base_color = {number(factor[0], 1), number(factor[1], 1), number(factor[2], 1), number(factor[3], 1)};
    }
    material.metallic = std::clamp(number(pbr["metallicFactor"], 1.0f), 0.0f, 1.0f);
    material.roughness = std::clamp(number(pbr["roughnessFactor"], 1.0f), 0.0f, 1.0f);
    material.base_color_texture = texture_image(pbr["baseColorTexture"]);
    material.normal_texture = texture_image(m["normalTexture"]);
    const Json& emissive = m["emissiveFactor"];
    if (emissive.is_array() && emissive.size() == 3) {
      material.emissive = {number(emissive[0], 0), number(emissive[1], 0), number(emissive[2], 0)};
    }
    material.double_sided = m["doubleSided"].as_bool(false);
    if (m["alphaMode"].as_string() == "OPAQUE" || m["alphaMode"].is_null()) material.base_color.a = 1.0f;
    material_names.push_back(material.name);
    mesh.materials.push_back(std::move(material));
  }

  // Scene graph: roots of the chosen scene (or every parentless node).
  const Json& nodes = root["nodes"];
  std::vector<std::int64_t> roots;
  const std::int64_t scene_index = root.contains("scene") ? ref(root, "scene") : 0;
  const Json& scenes = root["scenes"];
  if (scene_index >= 0 && static_cast<std::size_t>(scene_index) < scenes.size()) {
    for (const Json& n : scenes[static_cast<std::size_t>(scene_index)]["nodes"].elements()) roots.push_back(n.as_int64(-1));
  } else {
    std::vector<bool> child(nodes.size(), false);
    for (const Json& node : nodes.elements()) {
      for (const Json& c : node["children"].elements()) {
        const std::int64_t k = c.as_int64(-1);
        if (k >= 0 && static_cast<std::size_t>(k) < child.size()) child[k] = true;
      }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (!child[i]) roots.push_back(static_cast<std::int64_t>(i));
    }
  }
  if (roots.empty() && root["meshes"].size() > 0) {
    // No scene graph at all: show every mesh once.
    for (std::size_t i = 0; i < root["meshes"].size(); ++i) roots.push_back(-1 - static_cast<std::int64_t>(i));
  }

  std::size_t points_only = 0, triangle_primitives = 0, skipped = 0;
  std::size_t visited_nodes = 0;
  struct Item {
    std::int64_t node;
    ir::Mat4 parent;
    int depth;
  };
  std::vector<Item> stack;
  for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back({*it, {}, 0});
  std::vector<std::uint8_t> on_path(nodes.size(), 0);
  while (!stack.empty()) {
    const Item item = stack.back();
    stack.pop_back();
    if (++visited_nodes > 100000) return fail(Status::Limit, "the glTF scene graph is too large");
    std::int64_t mesh_index = -1;
    ir::Mat4 world = item.parent;
    std::string node_name;
    if (item.node < 0) {
      mesh_index = -1 - item.node;
    } else {
      if (static_cast<std::size_t>(item.node) >= nodes.size()) return fail(Status::Decode, "glTF node {} does not exist", item.node);
      if (item.depth > 64) return fail(Status::Decode, "the glTF node hierarchy is too deep or cyclic");
      const Json& node = nodes[static_cast<std::size_t>(item.node)];
      world = item.parent * node_matrix(node);
      mesh_index = ref(node, "mesh");
      node_name = node["name"].as_string();
      const Json& children = node["children"];
      for (std::size_t k = children.size(); k > 0; --k) {
        const std::int64_t c = children[k - 1].as_int64(-1);
        if (c < 0) continue;
        if (c == item.node) return fail(Status::Decode, "glTF node {} is its own child", c);
        stack.push_back({c, world, item.depth + 1});
      }
    }
    if (mesh_index < 0) continue;
    if (static_cast<std::size_t>(mesh_index) >= root["meshes"].size()) {
      return fail(Status::Decode, "glTF mesh {} does not exist", mesh_index);
    }
    const Json& gmesh = root["meshes"][static_cast<std::size_t>(mesh_index)];
    if (node_name.empty()) node_name = gmesh["name"].as_string();
    const std::size_t object_first = mesh.face_count();
    for (const Json& primitive : gmesh["primitives"].elements()) {
      CVT_TRY(ctx.checkpoint(0.1 + 0.8 * static_cast<double>(visited_nodes) / (nodes.size() + 1.0)));
      const std::int64_t mode = primitive.contains("mode") ? primitive["mode"].as_int64(4) : 4;
      const Json& attributes = primitive["attributes"];
      const std::int64_t pos_accessor = ref(attributes, "POSITION");
      if (pos_accessor < 0) {
        ++skipped;
        continue;
      }
      if (mode == 1 || mode == 2 || mode == 3 || mode > 6) {
        ++skipped;
        continue;
      }
      std::size_t comps = 0;
      CVT_TRY_ASSIGN(const std::vector<float> pos, doc.floats(pos_accessor, comps));
      if (comps != 3) return fail(Status::Decode, "glTF POSITION must be VEC3");
      const std::size_t n = pos.size() / 3;
      ir::Mesh part;
      part.positions.reserve(n);
      for (std::size_t i = 0; i < n; ++i) part.positions.push_back({pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]});
      if (const std::int64_t a = ref(attributes, "NORMAL"); a >= 0) {
        CVT_TRY_ASSIGN(const std::vector<float> v, doc.floats(a, comps));
        if (comps == 3 && v.size() == n * 3) {
          for (std::size_t i = 0; i < n; ++i) part.normals.push_back({v[i * 3], v[i * 3 + 1], v[i * 3 + 2]});
        }
      }
      if (const std::int64_t a = ref(attributes, "TEXCOORD_0"); a >= 0) {
        CVT_TRY_ASSIGN(const std::vector<float> v, doc.floats(a, comps));
        if (comps == 2 && v.size() == n * 2) {
          for (std::size_t i = 0; i < n; ++i) part.texcoords.push_back({v[i * 2], 1.0f - v[i * 2 + 1]});
        }
      }
      if (const std::int64_t a = ref(attributes, "COLOR_0"); a >= 0) {
        CVT_TRY_ASSIGN(const std::vector<float> v, doc.floats(a, comps));
        if ((comps == 3 || comps == 4) && v.size() == n * comps) {
          for (std::size_t i = 0; i < n; ++i) {
            part.colors.push_back({v[i * comps], v[i * comps + 1], v[i * comps + 2], comps == 4 ? v[i * comps + 3] : 1.0f});
          }
        }
      }
      std::vector<std::uint32_t> idx;
      if (const std::int64_t a = ref(primitive, "indices"); a >= 0) {
        CVT_TRY_ASSIGN(idx, doc.indices(a));
      } else {
        idx.resize(n);
        for (std::size_t i = 0; i < n; ++i) idx[i] = static_cast<std::uint32_t>(i);
      }
      for (const std::uint32_t i : idx) {
        if (i >= n) return fail(Status::Decode, "a glTF index ({}) is out of range", i);
      }
      if (mode == 0) {
        ++points_only;
        if (mesh.indices.empty()) {
          transform(part, world);
          append(mesh, part);
        }
        continue;
      }
      ++triangle_primitives;
      if (mode == 4) {
        idx.resize(idx.size() - idx.size() % 3);
        part.indices = std::move(idx);
      } else if (mode == 5) {  // strip: alternate winding
        for (std::size_t i = 2; i < idx.size(); ++i) {
          if (i % 2 == 0) part.indices.insert(part.indices.end(), {idx[i - 2], idx[i - 1], idx[i]});
          else part.indices.insert(part.indices.end(), {idx[i - 1], idx[i - 2], idx[i]});
        }
      } else {  // fan
        for (std::size_t i = 2; i < idx.size(); ++i) part.indices.insert(part.indices.end(), {idx[0], idx[i - 1], idx[i]});
      }
      remove_unused_vertices(part);
      transform(part, world);
      const std::int64_t material = ref(primitive, "material");
      const std::string material_name =
          material >= 0 && static_cast<std::size_t>(material) < material_names.size() ? material_names[material] : "";
      part.groups.push_back({node_name, material_name, 0, part.face_count()});
      part.materials.clear();
      if (mesh.point_cloud) mesh = ir::Mesh{};  // faces win over loose points
      const auto materials = std::move(mesh.materials);
      const auto textures = std::move(mesh.textures);
      append(mesh, part);
      mesh.materials = materials;
      mesh.textures = textures;
    }
    if (mesh.face_count() > object_first) {
      mesh.objects.push_back({node_name, object_first, mesh.face_count() - object_first, world});
    }
  }
  if (skipped > 0) ctx.warn(std::format("{} glTF primitives (lines or without positions) were skipped.", skipped));
  if (triangle_primitives == 0 && points_only > 0) {
    mesh.point_cloud = true;
    mesh.objects.clear();
    mesh.groups.clear();
  }
  if (mesh.objects.size() == 1 && mesh.objects[0].transform.is_identity()) mesh.objects.clear();
  if (mesh.positions.empty()) return fail(Status::Decode, "the glTF file contains no geometry");
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- writing

// A float as the double whose shortest form equals the float's shortest form,
// so that the JSON says 0.1 rather than 0.10000000149011612.
Json jf(float value) {
  if (!std::isfinite(value)) return Json(0.0);
  std::string text;
  append_number(text, value);
  double out = 0;
  return parse_double(text, out) ? Json(out) : Json(static_cast<double>(value));
}

class GltfBuilder {
 public:
  explicit GltfBuilder(bool binary) : binary_(binary) {}

  // Appends data as a new buffer view and returns its index.
  std::size_t view(ByteView data, int target, std::size_t stride = 0) {
    bin_.align(4);
    Json v = Json::object({{"buffer", 0}, {"byteOffset", static_cast<std::uint64_t>(bin_.size())},
                           {"byteLength", static_cast<std::uint64_t>(data.size())}});
    if (stride != 0) v["byteStride"] = static_cast<std::uint64_t>(stride);
    if (target != 0) v["target"] = target;
    bin_.bytes(data);
    views_.push_back(std::move(v));
    return views_.size() - 1;
  }
  std::size_t accessor(Json a) {
    accessors_.push_back(std::move(a));
    return accessors_.size() - 1;
  }
  std::size_t float_accessor(const std::vector<float>& values, std::size_t comps, int target, bool minmax) {
    util::ByteWriter w(values.size() * 4);
    for (const float v : values) w.f32le(v);
    const std::size_t v = view(w.view(), target);
    Json a = Json::object({{"bufferView", static_cast<std::uint64_t>(v)}, {"componentType", ct_float},
                           {"count", static_cast<std::uint64_t>(values.size() / comps)},
                           {"type", comps == 2 ? "VEC2" : comps == 3 ? "VEC3" : "VEC4"}});
    if (minmax) add_minmax(a, values, comps);
    return accessor(std::move(a));
  }
  static void add_minmax(Json& a, const std::vector<float>& values, std::size_t comps) {
    std::vector<float> lo(comps, std::numeric_limits<float>::max()), hi(comps, std::numeric_limits<float>::lowest());
    for (std::size_t i = 0; i < values.size(); ++i) {
      lo[i % comps] = std::min(lo[i % comps], values[i]);
      hi[i % comps] = std::max(hi[i % comps], values[i]);
    }
    Json jl = Json::array(), jh = Json::array();
    for (std::size_t c = 0; c < comps; ++c) {
      jl.push_back(jf(lo[c]));
      jh.push_back(jf(hi[c]));
    }
    a["min"] = std::move(jl);
    a["max"] = std::move(jh);
  }

  util::ByteWriter& bin() { return bin_; }
  Json& views() { return views_; }
  Json& accessors() { return accessors_; }
  [[nodiscard]] bool binary() const { return binary_; }

 private:
  bool binary_;
  util::ByteWriter bin_;
  Json views_ = Json::array();
  Json accessors_ = Json::array();
};

constexpr WriterTraits gltf_traits() {
  WriterTraits traits;
  traits.triangles_only = true;
  traits.objects = true;
  traits.point_clouds = true;
  traits.up = WriterTraits::Up::Y;
  return traits;
}

Result<Bytes> write_gltf(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, gltf_traits()));
  if (mesh.unit_meters > 0.0 && mesh.unit_meters != 1.0) convert_units(mesh, 1.0);  // glTF is in metres
  const bool binary = ctx.format_id == "glb";
  const bool quantize = ctx.options.get_bool("quantize", false);
  GltfBuilder b(binary);
  Json root = Json::object();
  Json asset = Json::object({{"version", "2.0"}});
  if (!options::resolved_strip_metadata(ctx.options)) {
    asset["generator"] = "Convertible";
    if (const auto it = mesh.metadata.find("copyright"); it != mesh.metadata.end()) asset["copyright"] = it->second;
  }
  root["asset"] = std::move(asset);

  // Images (PNG and JPEG only in core glTF), textures, materials.
  Json images = Json::array(), textures = Json::array(), materials = Json::array();
  std::vector<int> texture_of_image(mesh.textures.size(), -1);
  for (std::size_t i = 0; i < mesh.textures.size(); ++i) {
    const ir::Texture& t = mesh.textures[i];
    const std::string mime = t.mime.empty() ? image_mime(t.data, t.uri) : t.mime;
    if (t.data.empty() || (mime != "image/png" && mime != "image/jpeg")) {
      if (!t.data.empty()) ctx.warn(std::format("Texture '{}' ({}) cannot be stored in glTF and was left out.", t.name, mime));
      continue;
    }
    Json image = Json::object();
    if (!t.name.empty()) image["name"] = t.name;
    if (binary) {
      image["bufferView"] = static_cast<std::uint64_t>(b.view(t.data, 0));
      image["mimeType"] = mime;
    } else {
      image["uri"] = std::format("data:{};base64,{}", mime, util::base64_encode(t.data));
    }
    images.push_back(std::move(image));
    textures.push_back(Json::object({{"source", static_cast<std::uint64_t>(images.size() - 1)}, {"sampler", 0}}));
    texture_of_image[i] = static_cast<int>(textures.size() - 1);
  }
  std::map<std::string, std::size_t> material_index;
  for (const ir::Material& m : mesh.materials) {
    Json pbr = Json::object({{"baseColorFactor", Json::array({jf(m.base_color.r), jf(m.base_color.g), jf(m.base_color.b),
                                                               jf(m.base_color.a)})},
                             {"metallicFactor", jf(m.metallic)},
                             {"roughnessFactor", jf(m.roughness)}});
    if (m.base_color_texture >= 0 && texture_of_image[m.base_color_texture] >= 0) {
      pbr["baseColorTexture"] = Json::object({{"index", texture_of_image[m.base_color_texture]}});
    }
    Json material = Json::object({{"name", m.name}, {"pbrMetallicRoughness", std::move(pbr)}});
    if (m.normal_texture >= 0 && texture_of_image[m.normal_texture] >= 0) {
      material["normalTexture"] = Json::object({{"index", texture_of_image[m.normal_texture]}});
    }
    if (m.emissive.x > 0 || m.emissive.y > 0 || m.emissive.z > 0) {
      material["emissiveFactor"] = Json::array({jf(std::min(m.emissive.x, 1.0f)), jf(std::min(m.emissive.y, 1.0f)),
                                                jf(std::min(m.emissive.z, 1.0f))});
    }
    if (m.base_color.a < 1.0f) material["alphaMode"] = "BLEND";
    if (m.double_sided) material["doubleSided"] = true;
    material_index.emplace(m.name, materials.size());
    materials.push_back(std::move(material));
  }

  Json meshes = Json::array(), nodes = Json::array(), scene_nodes = Json::array();
  const std::vector<ObjectPart> parts = split_objects(mesh, true);
  for (std::size_t p = 0; p < parts.size(); ++p) {
    CVT_TRY(ctx.checkpoint(0.8 + 0.15 * static_cast<double>(p) / parts.size()));
    const ir::Mesh& part = parts[p].mesh;
    if (part.positions.empty()) continue;
    const std::size_t n = part.positions.size();
    Json attributes = Json::object();
    ir::Mat4 dequantize;
    if (quantize) {
      // Positions as unsigned 16-bit integers over the part's box; the node
      // matrix maps them back (KHR_mesh_quantization).
      const Bounds box = bounds(part);
      const ir::Vec3 size = box.size();
      const float sx = size.x > 0 ? size.x / 65535.0f : 1.0f, sy = size.y > 0 ? size.y / 65535.0f : 1.0f,
                  sz = size.z > 0 ? size.z / 65535.0f : 1.0f;
      dequantize = translation(box.min.x, box.min.y, box.min.z) * scaling(sx, sy, sz);
      util::ByteWriter w(n * 8);
      std::array<std::uint16_t, 3> lo{65535, 65535, 65535}, hi{0, 0, 0};
      for (const ir::Vec3& v : part.positions) {
        const std::array<std::uint16_t, 3> q = {
            static_cast<std::uint16_t>(std::lround(std::clamp((v.x - box.min.x) / sx, 0.0f, 65535.0f))),
            static_cast<std::uint16_t>(std::lround(std::clamp((v.y - box.min.y) / sy, 0.0f, 65535.0f))),
            static_cast<std::uint16_t>(std::lround(std::clamp((v.z - box.min.z) / sz, 0.0f, 65535.0f)))};
        for (int c = 0; c < 3; ++c) {
          lo[c] = std::min(lo[c], q[c]);
          hi[c] = std::max(hi[c], q[c]);
        }
        w.u16le(q[0]).u16le(q[1]).u16le(q[2]).u16le(0);
      }
      const std::size_t view = b.view(w.view(), 34962, 8);
      attributes["POSITION"] = static_cast<std::uint64_t>(b.accessor(Json::object(
          {{"bufferView", static_cast<std::uint64_t>(view)}, {"componentType", ct_ushort},
           {"count", static_cast<std::uint64_t>(n)}, {"type", "VEC3"},
           {"min", Json::array({lo[0], lo[1], lo[2]})}, {"max", Json::array({hi[0], hi[1], hi[2]})}})));
      if (!part.normals.empty()) {
        util::ByteWriter nw(n * 4);
        for (const ir::Vec3& v : part.normals) {
          const auto q = [](float c) { return static_cast<std::int8_t>(std::lround(std::clamp(c, -1.0f, 1.0f) * 127.0f)); };
          nw.i8(q(v.x)).i8(q(v.y)).i8(q(v.z)).i8(0);
        }
        const std::size_t nview = b.view(nw.view(), 34962, 4);
        attributes["NORMAL"] = static_cast<std::uint64_t>(b.accessor(Json::object(
            {{"bufferView", static_cast<std::uint64_t>(nview)}, {"componentType", ct_byte}, {"normalized", true},
             {"count", static_cast<std::uint64_t>(n)}, {"type", "VEC3"}})));
      }
    } else {
      std::vector<float> values;
      values.reserve(n * 3);
      for (const ir::Vec3& v : part.positions) values.insert(values.end(), {v.x, v.y, v.z});
      attributes["POSITION"] = static_cast<std::uint64_t>(b.float_accessor(values, 3, 34962, true));
      if (!part.normals.empty()) {
        values.clear();
        for (const ir::Vec3& v : part.normals) values.insert(values.end(), {v.x, v.y, v.z});
        attributes["NORMAL"] = static_cast<std::uint64_t>(b.float_accessor(values, 3, 34962, false));
      }
    }
    if (!part.texcoords.empty()) {
      bool unit_range = quantize;
      for (const ir::Vec2& t : part.texcoords) {
        unit_range = unit_range && t.x >= 0 && t.x <= 1 && t.y >= 0 && t.y <= 1;
      }
      if (unit_range) {
        util::ByteWriter w(n * 4);
        for (const ir::Vec2& t : part.texcoords) {
          w.u16le(static_cast<std::uint16_t>(std::lround(t.x * 65535.0f)));
          w.u16le(static_cast<std::uint16_t>(std::lround((1.0f - t.y) * 65535.0f)));
        }
        const std::size_t view = b.view(w.view(), 34962);
        attributes["TEXCOORD_0"] = static_cast<std::uint64_t>(b.accessor(Json::object(
            {{"bufferView", static_cast<std::uint64_t>(view)}, {"componentType", ct_ushort}, {"normalized", true},
             {"count", static_cast<std::uint64_t>(n)}, {"type", "VEC2"}})));
      } else {
        std::vector<float> values;
        for (const ir::Vec2& t : part.texcoords) values.insert(values.end(), {t.x, 1.0f - t.y});
        attributes["TEXCOORD_0"] = static_cast<std::uint64_t>(b.float_accessor(values, 2, 34962, false));
      }
    }
    if (!part.colors.empty()) {
      const bool alpha = std::any_of(part.colors.begin(), part.colors.end(), [](const ir::Color& c) { return c.a < 1.0f; });
      std::vector<float> values;
      for (const ir::Color& c : part.colors) {
        values.insert(values.end(), {c.r, c.g, c.b});
        if (alpha) values.push_back(c.a);
      }
      attributes["COLOR_0"] = static_cast<std::uint64_t>(b.float_accessor(values, alpha ? 4 : 3, 34962, false));
    }

    // One primitive per material run.
    Json primitives = Json::array();
    const std::size_t faces = part.face_count();
    std::vector<std::string> face_material(faces);
    for (const ir::MeshGroup& g : part.groups) {
      for (std::size_t f = g.first_face; f < g.first_face + g.face_count && f < faces; ++f) face_material[f] = g.material;
    }
    std::vector<std::string> order;
    for (const std::string& name : face_material) {
      if (std::find(order.begin(), order.end(), name) == order.end()) order.push_back(name);
    }
    if (part.point_cloud) {
      primitives.push_back(Json::object({{"attributes", attributes}, {"mode", 0}}));
    }
    for (const std::string& name : order) {
      std::vector<std::uint32_t> idx;
      for (std::size_t f = 0; f < faces; ++f) {
        if (face_material[f] != name) continue;
        idx.insert(idx.end(), part.indices.begin() + static_cast<std::ptrdiff_t>(f * 3),
                   part.indices.begin() + static_cast<std::ptrdiff_t>(f * 3 + 3));
      }
      const bool wide = n > 65535;
      util::ByteWriter w(idx.size() * (wide ? 4 : 2));
      std::uint32_t lo = std::numeric_limits<std::uint32_t>::max(), hi = 0;
      for (const std::uint32_t i : idx) {
        if (wide) w.u32le(i);
        else w.u16le(static_cast<std::uint16_t>(i));
        lo = std::min(lo, i);
        hi = std::max(hi, i);
      }
      const std::size_t view = b.view(w.view(), 34963);
      const std::size_t accessor = b.accessor(Json::object(
          {{"bufferView", static_cast<std::uint64_t>(view)}, {"componentType", wide ? ct_uint : ct_ushort},
           {"count", static_cast<std::uint64_t>(idx.size())}, {"type", "SCALAR"}}));
      Json primitive = Json::object({{"attributes", attributes}, {"indices", static_cast<std::uint64_t>(accessor)}, {"mode", 4}});
      if (const auto it = material_index.find(name); it != material_index.end()) {
        primitive["material"] = static_cast<std::uint64_t>(it->second);
      }
      primitives.push_back(std::move(primitive));
    }
    if (primitives.empty()) continue;
    Json gmesh = Json::object({{"primitives", std::move(primitives)}});
    if (!parts[p].name.empty()) gmesh["name"] = parts[p].name;
    meshes.push_back(std::move(gmesh));
    Json node = Json::object({{"mesh", static_cast<std::uint64_t>(meshes.size() - 1)}});
    if (!parts[p].name.empty()) node["name"] = parts[p].name;
    const ir::Mat4 matrix = parts[p].transform * dequantize;
    if (!matrix.is_identity()) {
      Json m = Json::array();
      for (const float v : matrix.m) m.push_back(jf(v));
      node["matrix"] = std::move(m);
    }
    nodes.push_back(std::move(node));
    scene_nodes.push_back(static_cast<std::uint64_t>(nodes.size() - 1));
  }
  if (meshes.empty()) return fail(Status::Encode, "the model has no geometry");
  if (quantize) {
    root["extensionsUsed"] = Json::array({"KHR_mesh_quantization"});
    root["extensionsRequired"] = Json::array({"KHR_mesh_quantization"});
  }
  root["scene"] = 0;
  root["scenes"] = Json::array({Json::object({{"nodes", std::move(scene_nodes)}})});
  root["nodes"] = std::move(nodes);
  root["meshes"] = std::move(meshes);
  if (!materials.empty()) root["materials"] = std::move(materials);
  if (!textures.empty()) {
    root["textures"] = std::move(textures);
    root["images"] = std::move(images);
    root["samplers"] = Json::array({Json::object({{"magFilter", 9729}, {"minFilter", 9987}})});
  }
  root["accessors"] = std::move(b.accessors());
  root["bufferViews"] = std::move(b.views());
  b.bin().align(4);
  const Bytes bin = b.bin().take();
  Json buffer = Json::object({{"byteLength", static_cast<std::uint64_t>(bin.size())}});
  if (!binary) buffer["uri"] = "data:application/octet-stream;base64," + util::base64_encode(bin);
  root["buffers"] = Json::array({std::move(buffer)});

  if (!binary) {
    std::string text = util::dump_json(root, 2);
    text += '\n';
    CVT_TRY(ctx.limits.check_output(text.size()));
    return to_bytes(text);
  }
  std::string json = util::dump_json(root);
  while (json.size() % 4 != 0) json.push_back(' ');
  const std::uint64_t total = 12 + 8 + json.size() + 8 + bin.size();
  if (total > 0xFFFFFFFFull) return fail(Status::Encode, "the model is too large for a GLB file");
  CVT_TRY(ctx.limits.check_output(total));
  util::ByteWriter out(static_cast<std::size_t>(total));
  out.u32le(glb_magic).u32le(2).u32le(static_cast<std::uint32_t>(total));
  out.u32le(static_cast<std::uint32_t>(json.size())).u32le(chunk_json).text(json);
  out.u32le(static_cast<std::uint32_t>(bin.size())).u32le(chunk_bin).bytes(bin);
  return out.take();
}

int sniff_gltf(ByteView data, std::string_view /*filename*/) {
  const std::string_view head = text_head(data, 4096);
  const std::size_t start = head.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos || head[start] != '{') return 0;
  if (head.find("\"asset\"") == std::string_view::npos) return 0;
  return head.find("\"version\"") != std::string_view::npos ? 90 : 70;
}

std::vector<OptionSpec> gltf_options() {
  std::vector<OptionSpec> specs = mesh_write_options(gltf_traits());
  specs.push_back(OptionSpec::boolean("quantize", "Quantise geometry", false,
                                      "Store positions, normals and texture coordinates as 16/8-bit integers "
                                      "(KHR_mesh_quantization) for smaller files."));
  return specs;
}

}  // namespace

void register_gltf(Registry& registry) {
  FormatInfo gltf;
  gltf.id = "gltf";
  gltf.name = "glTF model (JSON)";
  gltf.description = "Khronos glTF 2.0 scene in JSON form with embedded buffers.";
  gltf.category = Category::Model3D;
  gltf.extensions = {"gltf"};
  gltf.mime = "model/gltf+json";
  gltf.popularity = 65;
  gltf.sniff = sniff_gltf;
  registry.add_format(std::move(gltf));

  FormatInfo glb;
  glb.id = "glb";
  glb.name = "glTF model (binary)";
  glb.description = "Khronos glTF 2.0 scene packed into one binary file.";
  glb.category = Category::Model3D;
  glb.extensions = {"glb"};
  glb.mime = "model/gltf-binary";
  glb.popularity = 68;
  glb.signatures = {Signature::at(0, "'glTF' 02 00 00 00")};
  registry.add_format(std::move(glb));

  for (const char* id : {"glb", "gltf"}) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Mesh;
    codec.read = read_gltf;
    codec.write = write_gltf;
    codec.write_options = gltf_options();
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::mesh
