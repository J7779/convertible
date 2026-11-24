// Convertible core - mesh family: COLLADA (.dae) 1.4/1.5.
//
// Reader: geometries with <triangles>, <polylist>, <polygons>, <trifans>
// and <tristrips> using any input/offset layout, node hierarchies with
// matrix/translate/rotate/scale transforms (instance_node with cycle
// protection), bound materials from profile_COMMON effects, asset units and
// up axis. Writer: valid COLLADA 1.4.1 with one node per object.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
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
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::mesh {

namespace {

std::string_view strip_hash(std::string_view url) { return url.starts_with('#') ? url.substr(1) : url; }

struct Source {
  std::vector<float> values;
  std::size_t stride = 1;
  std::size_t count = 0;
  std::size_t offset = 0;
};

struct Input {
  std::string semantic;
  std::string source;
  std::size_t offset = 0;
  std::int64_t set = 0;
};

struct EffectInfo {
  ir::Color diffuse{0.8f, 0.8f, 0.8f, 1.0f};
  ir::Vec3 emission{0, 0, 0};
  float roughness = 1.0f;
  std::string image;  // image id of the diffuse texture
  float transparency = 1.0f;
};

struct CornerKey {
  std::uint32_t v[4];
  friend bool operator==(const CornerKey&, const CornerKey&) = default;
};
struct CornerKeyHash {
  std::size_t operator()(const CornerKey& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::uint32_t x : k.v) h = (h ^ x) * 1099511628211ull;
    return static_cast<std::size_t>(h);
  }
};

struct Geometry {
  // Corners of all primitives, each a full index tuple, plus per-primitive material symbols.
  ir::Mesh mesh;
  std::vector<std::string> symbols;  // per group
};

class ColladaReader {
 public:
  explicit ColladaReader(ReadContext& ctx) : ctx_(ctx) {}

  Result<ir::Mesh> read(const util::XmlNode& root) {
    // Index every element with an id (the document is the only namespace for urls).
    for (const util::XmlNode node : root.descendants()) {
      if (!node.is_element()) continue;
      if (const auto id = node.attribute("id")) by_id_.emplace(std::string(id.value()), node);
      if (++elements_ > 5'000'000) return fail(Status::Limit, "the COLLADA document is too large");
    }
    ir::Mesh out;
    if (const util::XmlNode asset = root.child_local("asset")) {
      if (const util::XmlNode unit = asset.child_local("unit")) out.unit_meters = unit.attribute("meter").as_double(1.0);
      up_axis_ = util::trim(asset.child_text("up_axis"));
      for (const util::XmlNode contributor : asset.children_local("contributor")) {
        if (const std::string t(util::trim(contributor.child_text("authoring_tool"))); !t.empty()) out.metadata["tool"] = t;
        if (const std::string a(util::trim(contributor.child_text("author"))); !a.empty()) out.metadata["author"] = a;
        if (const std::string c(util::trim(contributor.child_text("copyright"))); !c.empty()) out.metadata["copyright"] = c;
      }
      if (const std::string t(util::trim(asset.child_text("title"))); !t.empty()) out.metadata["title"] = t;
    }
    // Scene: the instanced visual scene, else the first one.
    util::XmlNode scene;
    if (const util::XmlNode s = root.child_local("scene")) {
      if (const util::XmlNode inst = s.child_local("instance_visual_scene")) scene = find(inst.attribute("url").value());
    }
    if (!scene) {
      if (const util::XmlNode lib = root.child_local("library_visual_scenes")) scene = lib.child_local("visual_scene");
    }
    if (scene) {
      for (const util::XmlNode node : scene.children_local("node")) CVT_TRY(visit(node, {}, 0, out));
    } else {
      // No scene: every geometry once.
      if (const util::XmlNode lib = root.child_local("library_geometries")) {
        for (const util::XmlNode g : lib.children_local("geometry")) CVT_TRY(instance(g, {}, {}, std::string(g.attribute("name").value()), out));
      }
    }
    if (out.objects.size() == 1 && out.objects[0].transform.is_identity()) out.objects.clear();
    return out;
  }

  [[nodiscard]] const std::string& up_axis() const { return up_axis_; }

 private:
  util::XmlNode find(std::string_view url) const {
    const auto it = by_id_.find(std::string(strip_hash(url)));
    return it == by_id_.end() ? util::XmlNode() : it->second;
  }

  Result<ir::Mat4> node_transform(const util::XmlNode& node) {
    ir::Mat4 m;
    for (const util::XmlNode child : node.elements()) {
      const std::string_view name = child.local_name();
      if (name != "matrix" && name != "translate" && name != "rotate" && name != "scale" && name != "lookat") continue;
      CVT_TRY_ASSIGN(const std::vector<float> v, parse_float_list(ctx_, child.text(), "transform values"));
      if (name == "matrix" && v.size() == 16) {
        m = m * from_row_major(std::span<const float, 16>(v.data(), 16));
      } else if (name == "translate" && v.size() == 3) {
        m = m * translation(v[0], v[1], v[2]);
      } else if (name == "rotate" && v.size() == 4) {
        m = m * rotation(v[3], v[0], v[1], v[2]);
      } else if (name == "scale" && v.size() == 3) {
        m = m * scaling(v[0], v[1], v[2]);
      } else if (name == "lookat") {
        ctx_.warn("COLLADA <lookat> transforms were ignored.");
      }
    }
    return m;
  }

  Result<void> visit(const util::XmlNode& node, const ir::Mat4& parent, int depth, ir::Mesh& out) {
    if (depth > 64) return fail(Status::Decode, "the COLLADA node hierarchy is too deep or cyclic");
    if (++visits_ > 200000) return fail(Status::Limit, "the COLLADA scene has too many nodes");
    CVT_TRY_ASSIGN(const ir::Mat4 local, node_transform(node));
    const ir::Mat4 world = parent * local;
    std::string name(node.attribute("name").value());
    if (name.empty()) name = std::string(node.attribute("id").value());
    for (const util::XmlNode child : node.elements()) {
      const std::string_view kind = child.local_name();
      if (kind == "instance_geometry") {
        const util::XmlNode geometry = find(child.attribute("url").value());
        if (!geometry) {
          ctx_.warn(std::format("COLLADA geometry '{}' was not found.", child.attribute("url").value()));
          continue;
        }
        std::map<std::string, std::string> bindings;  // symbol -> material id
        if (const util::XmlNode bind = child.child_local("bind_material")) {
          if (const util::XmlNode tc = bind.child_local("technique_common")) {
            for (const util::XmlNode im : tc.children_local("instance_material")) {
              bindings[std::string(im.attribute("symbol").value())] = std::string(strip_hash(im.attribute("target").value()));
            }
          }
        }
        CVT_TRY(instance(geometry, world, bindings, name, out));
      } else if (kind == "node") {
        CVT_TRY(visit(child, world, depth + 1, out));
      } else if (kind == "instance_node") {
        const util::XmlNode target = find(child.attribute("url").value());
        if (target) CVT_TRY(visit(target, world, depth + 1, out));
      }
    }
    return {};
  }

  Result<void> instance(const util::XmlNode& geometry, const ir::Mat4& world, const std::map<std::string, std::string>& bindings,
                        const std::string& name, ir::Mesh& out) {
    const std::string id(geometry.attribute("id").value());
    auto it = geometries_.find(id);
    if (it == geometries_.end()) {
      CVT_TRY_ASSIGN(Geometry parsed, parse_geometry(geometry));
      it = geometries_.emplace(id, std::move(parsed)).first;
    }
    ir::Mesh part = it->second.mesh;
    for (std::size_t g = 0; g < part.groups.size(); ++g) {
      const std::string& symbol = it->second.symbols[g];
      const auto bound = bindings.find(symbol);
      const std::string material_id = bound != bindings.end() ? bound->second : symbol;
      part.groups[g].material = material(material_id, out);
      part.groups[g].name = name;
    }
    transform(part, world);
    const std::size_t first = out.face_count();
    const auto materials = std::move(out.materials);
    const auto textures = std::move(out.textures);
    part.materials.clear();
    append(out, part);
    out.materials = materials;
    out.textures = textures;
    if (out.face_count() > first) out.objects.push_back({name, first, out.face_count() - first, world});
    CVT_TRY(check_elements(ctx_, out.positions.size(), "vertices"));
    return {};
  }

  // Name of the ir::Material for a COLLADA material id (created on first use).
  std::string material(const std::string& id, ir::Mesh& out) {
    if (id.empty()) return {};
    if (const auto it = material_names_.find(id); it != material_names_.end()) return it->second;
    const util::XmlNode m = find(id);
    ir::Material material;
    material.name = m ? std::string(m.attribute("name").value()) : id;
    if (material.name.empty()) material.name = id;
    if (m) {
      if (const util::XmlNode inst = m.child_local("instance_effect")) {
        const EffectInfo effect = parse_effect(find(inst.attribute("url").value()));
        material.base_color = effect.diffuse;
        material.base_color.a = std::clamp(effect.diffuse.a * effect.transparency, 0.0f, 1.0f);
        material.emissive = effect.emission;
        material.roughness = effect.roughness;
        if (!effect.image.empty()) {
          const util::XmlNode image = find(effect.image);
          ir::Texture texture;
          texture.uri = image ? util::trim(image.child_text("init_from")) : std::string();
          if (image && texture.uri.empty()) {
            if (const util::XmlNode init = image.child_local("init_from")) texture.uri = util::trim(init.child_text("ref"));
          }
          texture.name = std::string(base_name(texture.uri));
          if (!texture.uri.empty()) {
            ctx_.warn(std::format("Texture '{}' is a separate file and was not included.", texture.name));
            material.base_color_texture = static_cast<int>(out.textures.size());
            out.textures.push_back(std::move(texture));
          }
        }
      }
    }
    std::string name = material.name;
    for (int k = 2; out.find_material(name) != nullptr; ++k) name = std::format("{}_{}", material.name, k);
    material.name = name;
    out.materials.push_back(std::move(material));
    material_names_[id] = name;
    return name;
  }

  EffectInfo parse_effect(const util::XmlNode& effect) {
    EffectInfo info;
    if (!effect) return info;
    const util::XmlNode profile = effect.child_local("profile_COMMON");
    if (!profile) return info;
    std::map<std::string, util::XmlNode> params;
    for (const util::XmlNode p : profile.children_local("newparam")) params[std::string(p.attribute("sid").value())] = p;
    const util::XmlNode technique = profile.child_local("technique");
    if (!technique) return info;
    util::XmlNode shader;
    for (const util::XmlNode s : technique.elements()) {
      if (s.local_name() == "phong" || s.local_name() == "blinn" || s.local_name() == "lambert" || s.local_name() == "constant") {
        shader = s;
        break;
      }
    }
    if (!shader) return info;
    const auto color = [&](std::string_view name, ir::Color& out) {
      const util::XmlNode slot = shader.child_local(name);
      if (!slot) return false;
      if (const util::XmlNode c = slot.child_local("color")) {
        const std::string color_text = c.text();
        NumberScanner scan(color_text);
        float v[4] = {1, 1, 1, 1};
        int n = 0;
        while (n < 4 && scan.next_float(v[n])) ++n;
        if (n >= 3) out = {v[0], v[1], v[2], n == 4 ? v[3] : 1.0f};
        return true;
      }
      if (const util::XmlNode t = slot.child_local("texture")) {
        // sampler2D -> surface -> image (1.4.1), or an image id directly.
        std::string target(t.attribute("texture").value());
        for (int hop = 0; hop < 3; ++hop) {
          const auto it = params.find(target);
          if (it == params.end()) break;
          if (const util::XmlNode sampler = it->second.child_local("sampler2D")) {
            std::string next(util::trim(sampler.child_text("source")));
            if (next.empty()) {
              if (const util::XmlNode inst = sampler.child_local("instance_image")) next = std::string(strip_hash(inst.attribute("url").value()));
            }
            target = next;
          } else if (const util::XmlNode surface = it->second.child_local("surface")) {
            target = util::trim(surface.child_text("init_from"));
          } else {
            break;
          }
        }
        info.image = target;
        out = {1, 1, 1, 1};
        return true;
      }
      return false;
    };
    color("diffuse", info.diffuse);
    ir::Color emission{0, 0, 0, 1};
    if (color("emission", emission)) info.emission = {emission.r, emission.g, emission.b};
    if (const util::XmlNode s = shader.child_local("shininess")) {
      double ns = 0;
      if (parse_double(util::trim(s.child_text("float")), ns) && ns >= 0) {
        info.roughness = static_cast<float>(std::clamp(std::sqrt(2.0 / (ns + 2.0)), 0.0, 1.0));
      }
    }
    if (const util::XmlNode t = shader.child_local("transparency")) {
      double v = 1;
      if (parse_double(util::trim(t.child_text("float")), v)) info.transparency = static_cast<float>(std::clamp(v, 0.0, 1.0));
    }
    return info;
  }

  Result<const Source*> source(const util::XmlNode& mesh, std::string_view url) {
    const std::string id(strip_hash(url));
    if (const auto it = sources_.find(id); it != sources_.end()) return &it->second;
    const util::XmlNode node = find(id);
    if (!node || node.local_name() != "source") return fail(Status::Decode, "COLLADA source '{}' was not found", id);
    (void)mesh;
    Source s;
    const util::XmlNode array = node.child_local("float_array");
    if (!array) return fail(Status::Decode, "COLLADA source '{}' has no float_array", id);
    CVT_TRY_ASSIGN(s.values, parse_float_list(ctx_, array.text(), "COLLADA values"));
    s.stride = 1;
    s.count = s.values.size();
    if (const util::XmlNode tc = node.child_local("technique_common")) {
      if (const util::XmlNode acc = tc.child_local("accessor")) {
        s.stride = std::max<std::uint64_t>(1, acc.attribute("stride").as_uint64(1));
        s.count = static_cast<std::size_t>(acc.attribute("count").as_uint64(s.values.size() / s.stride));
        s.offset = static_cast<std::size_t>(acc.attribute("offset").as_uint64(0));
      }
    }
    if (s.stride > 64) return fail(Status::Decode, "COLLADA accessor stride is implausible");
    if (s.offset > s.values.size() || s.count > (s.values.size() - s.offset) / s.stride) {
      s.count = s.offset > s.values.size() ? 0 : (s.values.size() - s.offset) / s.stride;
      ctx_.warn("A COLLADA accessor declared more values than its array holds.");
    }
    return &sources_.emplace(id, std::move(s)).first->second;
  }

  Result<Geometry> parse_geometry(const util::XmlNode& geometry) {
    Geometry out;
    const util::XmlNode mesh = geometry.child_local("mesh");
    if (!mesh) return out;  // splines, convex meshes: nothing to draw
    // <vertices>: POSITION and per-vertex attributes.
    std::vector<Input> vertex_inputs;
    std::string vertices_id;
    if (const util::XmlNode vertices = mesh.child_local("vertices")) {
      vertices_id = std::string(vertices.attribute("id").value());
      for (const util::XmlNode in : vertices.children_local("input")) {
        vertex_inputs.push_back({std::string(in.attribute("semantic").value()), std::string(in.attribute("source").value()), 0, 0});
      }
    }
    std::unordered_map<CornerKey, std::uint32_t, CornerKeyHash> made;
    const Source* pos_source = nullptr;
    const Source* nrm_source = nullptr;
    const Source* uv_source = nullptr;
    const Source* col_source = nullptr;
    bool nrm_from_vertices = false, uv_from_vertices = false, col_from_vertices = false;
    for (const Input& in : vertex_inputs) {
      CVT_TRY_ASSIGN(const Source* s, source(mesh, in.source));
      if (in.semantic == "POSITION") pos_source = s;
      else if (in.semantic == "NORMAL") nrm_source = s, nrm_from_vertices = true;
      else if (in.semantic == "TEXCOORD") uv_source = s, uv_from_vertices = true;
      else if (in.semantic == "COLOR") col_source = s, col_from_vertices = true;
    }
    for (const util::XmlNode prim : mesh.elements()) {
      const std::string_view kind = prim.local_name();
      if (kind != "triangles" && kind != "polylist" && kind != "polygons" && kind != "trifans" && kind != "tristrips") {
        if (kind == "lines" || kind == "linestrips") ctx_.warn("COLLADA lines were not converted.");
        continue;
      }
      std::vector<Input> inputs;
      std::size_t stride = 0;
      int vertex_offset = -1, normal_offset = -1, uv_offset = -1, color_offset = -1;
      const Source* prim_nrm = nrm_from_vertices ? nrm_source : nullptr;
      const Source* prim_uv = uv_from_vertices ? uv_source : nullptr;
      const Source* prim_col = col_from_vertices ? col_source : nullptr;
      std::int64_t uv_set = std::numeric_limits<std::int64_t>::max();
      for (const util::XmlNode in : prim.children_local("input")) {
        Input input{std::string(in.attribute("semantic").value()), std::string(in.attribute("source").value()),
                    static_cast<std::size_t>(in.attribute("offset").as_uint64(0)), in.attribute("set").as_int64(0)};
        if (input.offset > 64) return fail(Status::Decode, "COLLADA input offset is implausible");
        stride = std::max(stride, input.offset + 1);
        if (input.semantic == "VERTEX") {
          vertex_offset = static_cast<int>(input.offset);
        } else if (input.semantic == "NORMAL" && normal_offset < 0) {
          normal_offset = static_cast<int>(input.offset);
          CVT_TRY_ASSIGN(prim_nrm, source(mesh, input.source));
        } else if (input.semantic == "TEXCOORD" && input.set < uv_set) {
          uv_set = input.set;
          uv_offset = static_cast<int>(input.offset);
          CVT_TRY_ASSIGN(prim_uv, source(mesh, input.source));
        } else if (input.semantic == "COLOR" && color_offset < 0) {
          color_offset = static_cast<int>(input.offset);
          CVT_TRY_ASSIGN(prim_col, source(mesh, input.source));
        }
      }
      if (vertex_offset < 0 || pos_source == nullptr || stride == 0) {
        return fail(Status::Decode, "a COLLADA primitive has no VERTEX input");
      }
      // Polygons as lists of corner tuples.
      std::vector<std::vector<std::int64_t>> polygons;
      std::vector<std::int64_t> all;
      const auto read_p = [&](const util::XmlNode& p) -> Result<std::vector<std::int64_t>> {
        return parse_int_list(ctx_, p.text(), "COLLADA indices");
      };
      if (kind == "polygons") {
        for (const util::XmlNode p : prim.children_local("p")) {
          CVT_TRY_ASSIGN(std::vector<std::int64_t> list, read_p(p));
          polygons.push_back(std::move(list));
        }
        if (prim.child_local("ph")) ctx_.warn("COLLADA polygons with holes were filled.");
        for (const util::XmlNode ph : prim.children_local("ph")) {
          if (const util::XmlNode p = ph.child_local("p")) {
            CVT_TRY_ASSIGN(std::vector<std::int64_t> list, read_p(p));
            polygons.push_back(std::move(list));
          }
        }
      } else if (kind == "trifans" || kind == "tristrips") {
        for (const util::XmlNode p : prim.children_local("p")) {
          CVT_TRY_ASSIGN(std::vector<std::int64_t> list, read_p(p));
          const std::size_t n = list.size() / stride;
          for (std::size_t i = 2; i < n; ++i) {
            std::vector<std::int64_t> tri;
            std::size_t a = 0, b = i - 1, c = i;
            if (kind == "tristrips") {
              a = i - 2;
              b = i - 1;
              if (i % 2 == 1) std::swap(a, b);
            }
            for (const std::size_t corner : {a, b, c}) {
              tri.insert(tri.end(), list.begin() + static_cast<std::ptrdiff_t>(corner * stride),
                         list.begin() + static_cast<std::ptrdiff_t>((corner + 1) * stride));
            }
            polygons.push_back(std::move(tri));
          }
        }
      } else {
        if (const util::XmlNode p = prim.child_local("p")) {
          CVT_TRY_ASSIGN(all, read_p(p));
        }
        std::vector<std::int64_t> vcount;
        if (kind == "polylist") {
          if (const util::XmlNode vc = prim.child_local("vcount")) {
            CVT_TRY_ASSIGN(vcount, parse_int_list(ctx_, vc.text(), "COLLADA vcount"));
          }
        }
        std::size_t at = 0;
        const std::size_t corners_total = all.size() / stride;
        if (kind == "triangles") vcount.assign(corners_total / 3, 3);
        for (const std::int64_t count : vcount) {
          if (count < 0 || static_cast<std::uint64_t>(count) > corners_total - std::min(at, corners_total)) {
            return fail(Status::Decode, "COLLADA vcount exceeds the index list");
          }
          polygons.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(at * stride),
                                all.begin() + static_cast<std::ptrdiff_t>((at + static_cast<std::size_t>(count)) * stride));
          at += static_cast<std::size_t>(count);
        }
      }
      const std::size_t first_face = out.mesh.face_count();
      for (const std::vector<std::int64_t>& polygon : polygons) {
        const std::size_t corners = polygon.size() / stride;
        if (corners < 3) continue;
        for (std::size_t c = 0; c < corners; ++c) {
          const auto get = [&](int offset) -> std::int64_t { return offset < 0 ? -1 : polygon[c * stride + static_cast<std::size_t>(offset)]; };
          const std::int64_t vi = get(vertex_offset);
          if (vi < 0 || static_cast<std::size_t>(vi) >= pos_source->count) {
            return fail(Status::Decode, "a COLLADA index ({}) is out of range", vi);
          }
          // Per-vertex attributes from <vertices> use the vertex index.
          const std::int64_t ni = normal_offset >= 0 ? get(normal_offset) : (prim_nrm ? vi : -1);
          const std::int64_t ti = uv_offset >= 0 ? get(uv_offset) : (prim_uv ? vi : -1);
          const std::int64_t ci = color_offset >= 0 ? get(color_offset) : (prim_col ? vi : -1);
          const auto check = [](const Source* s, std::int64_t i) { return s == nullptr || i < 0 || static_cast<std::size_t>(i) < s->count; };
          if (!check(prim_nrm, ni) || !check(prim_uv, ti) || !check(prim_col, ci)) {
            return fail(Status::Decode, "a COLLADA attribute index is out of range");
          }
          const CornerKey key{{static_cast<std::uint32_t>(vi), static_cast<std::uint32_t>(ni), static_cast<std::uint32_t>(ti),
                         static_cast<std::uint32_t>(ci)}};
          const auto [it, inserted] = made.emplace(key, static_cast<std::uint32_t>(out.mesh.positions.size()));
          if (inserted) {
            const auto value = [](const Source* s, std::int64_t i, std::size_t k) {
              return s != nullptr && i >= 0 && k < s->stride ? s->values[s->offset + static_cast<std::size_t>(i) * s->stride + k] : 0.0f;
            };
            out.mesh.positions.push_back({value(pos_source, vi, 0), value(pos_source, vi, 1), value(pos_source, vi, 2)});
            out.mesh.normals.push_back({value(prim_nrm, ni, 0), value(prim_nrm, ni, 1), value(prim_nrm, ni, 2)});
            out.mesh.texcoords.push_back({value(prim_uv, ti, 0), value(prim_uv, ti, 1)});
            const float alpha = prim_col != nullptr && prim_col->stride >= 4 ? value(prim_col, ci, 3) : 1.0f;
            out.mesh.colors.push_back({prim_col ? value(prim_col, ci, 0) : 1.0f, prim_col ? value(prim_col, ci, 1) : 1.0f,
                                       prim_col ? value(prim_col, ci, 2) : 1.0f, alpha});
            any_normals_ = any_normals_ || prim_nrm != nullptr;
            any_uvs_ = any_uvs_ || prim_uv != nullptr;
            any_colors_ = any_colors_ || prim_col != nullptr;
          }
          out.mesh.indices.push_back(it->second);
        }
        out.mesh.face_sizes.push_back(static_cast<std::uint32_t>(corners));
        CVT_TRY(check_elements(ctx_, out.mesh.indices.size(), "face corners"));
      }
      out.mesh.groups.push_back({{}, {}, first_face, out.mesh.face_count() - first_face});
      out.symbols.emplace_back(prim.attribute("material").value());
    }
    if (!any_normals_) out.mesh.normals.clear();
    if (!any_uvs_) out.mesh.texcoords.clear();
    if (!any_colors_) out.mesh.colors.clear();
    any_normals_ = any_uvs_ = any_colors_ = false;
    return out;
  }

  ReadContext& ctx_;
  std::unordered_map<std::string, util::XmlNode> by_id_;
  std::map<std::string, Geometry> geometries_;
  std::map<std::string, Source> sources_;
  std::map<std::string, std::string> material_names_;
  std::string up_axis_;
  std::size_t elements_ = 0;
  std::size_t visits_ = 0;
  bool any_normals_ = false, any_uvs_ = false, any_colors_ = false;
};

Result<AnyIR> read_dae(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const util::XmlDocument doc, util::parse_xml(ctx.input));
  const util::XmlNode root = doc.root();
  if (!root || root.local_name() != "COLLADA") return fail(Status::Decode, "not a COLLADA document");
  CVT_TRY(ctx.checkpoint(0.3, "Parsing"));
  ColladaReader reader(ctx);
  CVT_TRY_ASSIGN(ir::Mesh mesh, reader.read(root));
  if (mesh.positions.empty()) return fail(Status::Decode, "the COLLADA file contains no geometry");
  const std::string& up = reader.up_axis();
  if (up == "Z_UP") {
    mesh.source_up_axis = "Z";
    convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  } else if (up == "X_UP") {
    mesh.source_up_axis = "X";
    transform(mesh, rotation(90.0f, 0, 0, 1));
    for (ir::MeshObject& o : mesh.objects) o.transform = rotation(90.0f, 0, 0, 1) * o.transform;
  } else {
    mesh.source_up_axis = "Y";
  }
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- writing

constexpr WriterTraits dae_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.objects = true;
  traits.up = WriterTraits::Up::Y;
  return traits;
}

std::string xml_id(std::string_view text, std::string_view fallback) {
  std::string out;
  for (const char c : text) {
    const bool ok = util::is_ascii_alpha(c) || util::is_ascii_digit(c) || c == '_' || c == '-' || c == '.';
    out.push_back(ok ? c : '_');
  }
  if (out.empty() || util::is_ascii_digit(out.front()) || out.front() == '-' || out.front() == '.') out = std::string(fallback) + out;
  return out;
}

void float_array(std::string& xml, std::string_view id, const std::vector<float>& values, std::size_t stride,
                 std::initializer_list<std::string_view> params, int precision) {
  xml += std::format("        <source id=\"{0}\">\n          <float_array id=\"{0}-array\" count=\"{1}\">", id, values.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) xml += ' ';
    append_number(xml, values[i], precision);
  }
  xml += std::format("</float_array>\n          <technique_common>\n            <accessor source=\"#{}-array\" count=\"{}\" stride=\"{}\">\n",
                     id, values.size() / stride, stride);
  for (const std::string_view p : params) xml += std::format("              <param name=\"{}\" type=\"float\"/>\n", p);
  xml += "            </accessor>\n          </technique_common>\n        </source>\n";
}

Result<Bytes> write_dae(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, dae_traits()));
  const UnitChoice unit = resolve_unit(mesh, ctx, 1.0);
  if (unit.scale != 1.0f) {
    scale(mesh, unit.scale);
    for (ir::MeshObject& o : mesh.objects) {
      for (int r = 0; r < 3; ++r) o.transform.m[12 + r] *= unit.scale;
    }
  }
  const int precision = write_precision(ctx);
  const bool strip = options::resolved_strip_metadata(ctx.options);
  std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">\n  <asset>\n";
  if (!strip && !mesh.metadata.empty()) {
    xml += "    <contributor>\n";
    if (const auto it = mesh.metadata.find("author"); it != mesh.metadata.end()) {
      xml += std::format("      <author>{}</author>\n", util::xml_escape(it->second));
    }
    if (const auto it = mesh.metadata.find("tool"); it != mesh.metadata.end()) {
      xml += std::format("      <authoring_tool>{}</authoring_tool>\n", util::xml_escape(it->second));
    }
    if (const auto it = mesh.metadata.find("copyright"); it != mesh.metadata.end()) {
      xml += std::format("      <copyright>{}</copyright>\n", util::xml_escape(it->second));
    }
    xml += "    </contributor>\n";
  }
  // The schema requires both dates; a fixed value keeps output reproducible and private.
  xml += "    <created>1970-01-01T00:00:00Z</created>\n    <modified>1970-01-01T00:00:00Z</modified>\n";
  std::string meters;
  append_number(meters, unit.meters);
  xml += std::format("    <unit name=\"{}\" meter=\"{}\"/>\n    <up_axis>Y_UP</up_axis>\n  </asset>\n",
                     unit.meters == 1.0 ? "meter" : unit.meters == 0.01 ? "centimeter" : unit.meters == 0.001 ? "millimeter" : "unit",
                     meters);

  // Images, effects and materials.
  std::vector<std::string> material_ids;
  if (!mesh.materials.empty()) {
    bool any_texture = false;
    std::string images = "  <library_images>\n";
    for (std::size_t t = 0; t < mesh.textures.size(); ++t) {
      const ir::Texture& texture = mesh.textures[t];
      std::string file = texture.uri.empty() ? texture.name : std::string(base_name(texture.uri));
      if (file.empty()) file = std::format("texture_{}.png", t);
      images += std::format("    <image id=\"image-{}\" name=\"{}\">\n      <init_from>{}</init_from>\n    </image>\n", t,
                            util::xml_escape(xml_id(file, "image"), true), util::xml_escape(file));
      any_texture = true;
    }
    images += "  </library_images>\n";
    if (any_texture) {
      xml += images;
      ctx.warn("COLLADA refers to textures as separate files; the images themselves were not written.");
    }
    xml += "  <library_effects>\n";
    for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
      const ir::Material& m = mesh.materials[i];
      const std::string id = std::format("material-{}", i);
      material_ids.push_back(id);
      xml += std::format("    <effect id=\"{}-effect\">\n      <profile_COMMON>\n", id);
      const bool textured = m.base_color_texture >= 0;
      if (textured) {
        xml += std::format("        <newparam sid=\"surface-{0}\">\n          <surface type=\"2D\">\n            <init_from>image-{0}</init_from>\n"
                           "          </surface>\n        </newparam>\n        <newparam sid=\"sampler-{0}\">\n          <sampler2D>\n"
                           "            <source>surface-{0}</source>\n          </sampler2D>\n        </newparam>\n",
                           m.base_color_texture);
      }
      xml += "        <technique sid=\"common\">\n          <phong>\n";
      const auto color = [&](std::string_view slot, float r, float g, float b, float a) {
        std::string text;
        for (const float c : {r, g, b, a}) {
          if (!text.empty()) text += ' ';
          append_number(text, c, 6);
        }
        xml += std::format("            <{0}>\n              <color sid=\"{0}\">{1}</color>\n            </{0}>\n", slot, text);
      };
      color("emission", m.emissive.x, m.emissive.y, m.emissive.z, 1.0f);
      if (textured) {
        xml += std::format("            <diffuse>\n              <texture texture=\"sampler-{}\" texcoord=\"UVMap\"/>\n            </diffuse>\n",
                           m.base_color_texture);
      } else {
        color("diffuse", m.base_color.r, m.base_color.g, m.base_color.b, 1.0f);
      }
      color("specular", 0.5f, 0.5f, 0.5f, 1.0f);
      const float r = std::clamp(m.roughness, 0.02f, 1.0f);
      std::string shininess;
      append_number(shininess, std::clamp(2.0f / (r * r) - 2.0f, 0.0f, 1000.0f), 6);
      std::string transparency;
      append_number(transparency, m.base_color.a, 6);
      xml += std::format("            <shininess>\n              <float sid=\"shininess\">{}</float>\n            </shininess>\n"
                         "            <transparency>\n              <float sid=\"transparency\">{}</float>\n            </transparency>\n",
                         shininess, transparency);
      xml += "          </phong>\n        </technique>\n      </profile_COMMON>\n    </effect>\n";
    }
    xml += "  </library_effects>\n  <library_materials>\n";
    for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
      xml += std::format("    <material id=\"{0}\" name=\"{1}\">\n      <instance_effect url=\"#{0}-effect\"/>\n    </material>\n",
                         material_ids[i], util::xml_escape(mesh.materials[i].name, true));
    }
    xml += "  </library_materials>\n";
  }
  std::map<std::string, std::size_t> material_index;
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) material_index.emplace(mesh.materials[i].name, i);

  const std::vector<ObjectPart> parts = split_objects(mesh, true);
  xml += "  <library_geometries>\n";
  std::vector<std::vector<std::string>> part_materials(parts.size());
  for (std::size_t p = 0; p < parts.size(); ++p) {
    CVT_TRY(ctx.checkpoint(0.8 + 0.15 * static_cast<double>(p) / static_cast<double>(parts.size())));
    const ir::Mesh& part = parts[p].mesh;
    const std::string gid = std::format("geometry-{}", p);
    xml += std::format("    <geometry id=\"{}\" name=\"{}\">\n      <mesh>\n", gid,
                       util::xml_escape(parts[p].name.empty() ? gid : parts[p].name, true));
    std::vector<float> values;
    for (const ir::Vec3& v : part.positions) values.insert(values.end(), {v.x, v.y, v.z});
    float_array(xml, gid + "-positions", values, 3, {"X", "Y", "Z"}, precision);
    if (!part.normals.empty()) {
      values.clear();
      for (const ir::Vec3& v : part.normals) values.insert(values.end(), {v.x, v.y, v.z});
      float_array(xml, gid + "-normals", values, 3, {"X", "Y", "Z"}, precision);
    }
    if (!part.texcoords.empty()) {
      values.clear();
      for (const ir::Vec2& v : part.texcoords) values.insert(values.end(), {v.x, v.y});
      float_array(xml, gid + "-uvs", values, 2, {"S", "T"}, precision);
    }
    if (!part.colors.empty()) {
      values.clear();
      for (const ir::Color& c : part.colors) values.insert(values.end(), {c.r, c.g, c.b, c.a});
      float_array(xml, gid + "-colors", values, 4, {"R", "G", "B", "A"}, precision);
    }
    xml += std::format("        <vertices id=\"{0}-vertices\">\n          <input semantic=\"POSITION\" source=\"#{0}-positions\"/>\n", gid);
    if (!part.normals.empty()) xml += std::format("          <input semantic=\"NORMAL\" source=\"#{}-normals\"/>\n", gid);
    if (!part.texcoords.empty()) xml += std::format("          <input semantic=\"TEXCOORD\" source=\"#{}-uvs\"/>\n", gid);
    if (!part.colors.empty()) xml += std::format("          <input semantic=\"COLOR\" source=\"#{}-colors\"/>\n", gid);
    xml += "        </vertices>\n";
    // One polylist per material run.
    const std::size_t faces = part.face_count();
    std::vector<std::string> face_material(faces);
    for (const ir::MeshGroup& g : part.groups) {
      for (std::size_t f = g.first_face; f < g.first_face + g.face_count && f < faces; ++f) face_material[f] = g.material;
    }
    const std::vector<std::size_t> offsets = face_offsets(part);
    const bool triangles = part.face_sizes.empty();
    std::size_t f = 0;
    while (f < faces) {
      const std::string name = face_material[f];
      std::size_t end = f;
      while (end < faces && face_material[end] == name) ++end;
      const auto it = material_index.find(name);
      std::string symbol;
      if (it != material_index.end()) {
        symbol = material_ids[it->second];
        part_materials[p].push_back(symbol);
      }
      xml += std::format("        <{} count=\"{}\"", triangles ? "triangles" : "polylist", end - f);
      if (!symbol.empty()) xml += std::format(" material=\"{}\"", symbol);
      xml += std::format(">\n          <input semantic=\"VERTEX\" source=\"#{}-vertices\" offset=\"0\"/>\n", gid);
      if (!triangles) {
        xml += "          <vcount>";
        for (std::size_t k = f; k < end; ++k) xml += std::format("{}{}", k == f ? "" : " ", offsets[k + 1] - offsets[k]);
        xml += "</vcount>\n";
      }
      xml += "          <p>";
      for (std::size_t c = offsets[f]; c < offsets[end]; ++c) xml += std::format("{}{}", c == offsets[f] ? "" : " ", part.indices[c]);
      xml += std::format("</p>\n        </{}>\n", triangles ? "triangles" : "polylist");
      f = end;
    }
    xml += "      </mesh>\n    </geometry>\n";
  }
  xml += "  </library_geometries>\n  <library_visual_scenes>\n    <visual_scene id=\"scene\" name=\"scene\">\n";
  for (std::size_t p = 0; p < parts.size(); ++p) {
    const std::string name = parts[p].name.empty() ? std::format("node-{}", p) : parts[p].name;
    xml += std::format("      <node id=\"node-{}\" name=\"{}\" type=\"NODE\">\n", p, util::xml_escape(name, true));
    if (!parts[p].transform.is_identity()) {
      xml += "        <matrix sid=\"transform\">";
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          if (r + c > 0) xml += ' ';
          append_number(xml, parts[p].transform.at(r, c));
        }
      }
      xml += "</matrix>\n";
    }
    xml += std::format("        <instance_geometry url=\"#geometry-{}\">\n", p);
    if (!part_materials[p].empty()) {
      xml += "          <bind_material>\n            <technique_common>\n";
      std::vector<std::string> seen;
      for (const std::string& symbol : part_materials[p]) {
        if (std::find(seen.begin(), seen.end(), symbol) != seen.end()) continue;
        seen.push_back(symbol);
        xml += std::format("              <instance_material symbol=\"{0}\" target=\"#{0}\">\n"
                           "                <bind_vertex_input semantic=\"UVMap\" input_semantic=\"TEXCOORD\" input_set=\"0\"/>\n"
                           "              </instance_material>\n",
                           symbol);
      }
      xml += "            </technique_common>\n          </bind_material>\n";
    }
    xml += "        </instance_geometry>\n      </node>\n";
  }
  xml += "    </visual_scene>\n  </library_visual_scenes>\n  <scene>\n    <instance_visual_scene url=\"#scene\"/>\n  </scene>\n</COLLADA>\n";
  CVT_TRY(ctx.limits.check_output(xml.size()));
  return to_bytes(xml);
}

int sniff_dae(ByteView data, std::string_view /*filename*/) {
  const std::string_view head = text_head(data, 2048);
  const std::size_t at = head.find("<COLLADA");
  if (at == std::string_view::npos) return 0;
  return head.find("collada.org") != std::string_view::npos ? 95 : 85;
}

}  // namespace

void register_collada(Registry& registry) {
  FormatInfo info;
  info.id = "dae";
  info.name = "COLLADA model";
  info.description = "XML interchange format for 3D scenes (Khronos COLLADA).";
  info.category = Category::Model3D;
  info.extensions = {"dae"};
  info.mime = "model/vnd.collada+xml";
  info.popularity = 40;
  info.sniff = sniff_dae;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "dae";
  codec.kind = Kind::Mesh;
  codec.read = read_dae;
  codec.write = write_dae;
  codec.write_options = mesh_write_options(dae_traits());
  codec.write_options.push_back(unit_option());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::mesh
