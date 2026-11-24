// Convertible core - mesh family: Wavefront OBJ + MTL, and OBJ packed in a ZIP.
//
// OBJ indexes positions, texture coordinates and normals separately; the
// reader gives every distinct (v, vt, vn) triple one vertex of the IR's
// single index space. Materials live in separate MTL files, which a lone
// .obj cannot reach: "obj.zip" packs the model with its MTL files and
// textures so both directions keep them.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/mesh/common.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::mesh {

namespace {

struct ObjCorner {
  std::int64_t v = 0, vt = 0, vn = 0;  // 1-based after resolution; 0 = absent
  friend bool operator==(const ObjCorner&, const ObjCorner&) = default;
};
struct ObjCornerHash {
  std::size_t operator()(const ObjCorner& c) const noexcept {
    std::uint64_t h = static_cast<std::uint64_t>(c.v) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(c.vt) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(c.vn) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

// Callback used to fetch companion files (MTL, textures) by relative name.
using FileLookup = std::function<std::optional<Bytes>(std::string_view name)>;

std::string_view strip_comment(std::string_view line) {
  const std::size_t hash = line.find('#');
  return hash == std::string_view::npos ? line : line.substr(0, hash);
}

// Texture statements carry options ("-s 1 1 1 -clamp on file.png"); the file
// name is what is left after the options (it may contain spaces).
std::string texture_file(std::string_view args) {
  std::vector<std::string_view> parts = tokens(args);
  static const std::map<std::string_view, int, std::less<>> option_arity = {
      {"-blendu", 1}, {"-blendv", 1}, {"-bm", 1},  {"-boost", 1}, {"-cc", 1},    {"-clamp", 1},
      {"-imfchan", 1}, {"-mm", 2},    {"-o", 3},   {"-s", 3},     {"-t", 3},     {"-texres", 1},
      {"-type", 1}};
  std::size_t i = 0;
  while (i < parts.size()) {
    const auto it = option_arity.find(parts[i]);
    if (it == option_arity.end()) break;
    i += 1 + static_cast<std::size_t>(it->second);
    // -o/-s/-t accept 1 to 3 numbers.
    while (i < parts.size() && parts[i].front() != '-' && (it->first == "-o" || it->first == "-s" || it->first == "-t")) {
      float probe = 0;
      if (!parse_float(parts[i], probe)) break;
      ++i;
    }
  }
  if (i >= parts.size()) return {};
  const char* begin = parts[i].data();
  const char* end = parts.back().data() + parts.back().size();
  return std::string(util::trim(std::string_view(begin, static_cast<std::size_t>(end - begin))));
}

// Adds the materials of one MTL file; textures are fetched through `lookup`.
void parse_mtl(std::string_view text, std::string_view mtl_name, ir::Mesh& mesh, const FileLookup& lookup,
               const ReadContext& ctx) {
  LineReader lines(text);
  std::string_view raw;
  ir::Material* current = nullptr;
  std::map<std::string, int> texture_by_path;
  const auto texture = [&](std::string_view args) -> int {
    const std::string file = texture_file(args);
    if (file.empty()) return -1;
    const std::string path = resolve_relative(mtl_name, file);
    if (const auto it = texture_by_path.find(path); it != texture_by_path.end()) return it->second;
    ir::Texture tex;
    tex.name = std::string(base_name(file));
    tex.uri = file;
    if (lookup) {
      std::optional<Bytes> data = lookup(path);
      if (!data) data = lookup(base_name(file));
      if (data) {
        tex.data = std::move(*data);
        tex.mime = image_mime(tex.data, file);
      }
    }
    if (tex.data.empty()) ctx.warn(std::format("Texture '{}' was not found next to the model.", file));
    const int index = static_cast<int>(mesh.textures.size());
    mesh.textures.push_back(std::move(tex));
    texture_by_path[path] = index;
    return index;
  };
  const auto rgb = [](std::string_view rest, float fallback_alpha) {
    NumberScanner scan(rest, false);
    float r = 0, g = 0, b = 0;
    if (!scan.next_float(r)) return ir::Color{1, 1, 1, fallback_alpha};
    if (!scan.next_float(g)) g = r;
    if (!scan.next_float(b)) b = g;
    return ir::Color{r, g, b, fallback_alpha};
  };
  while (lines.next(raw)) {
    std::string_view line = util::trim(strip_comment(raw));
    std::string_view keyword = next_token(line);
    line = util::trim(line);
    if (keyword.empty()) continue;
    if (keyword == "newmtl") {
      ir::Material material;
      material.name = std::string(line);
      material.base_color = {0.8f, 0.8f, 0.8f, 1.0f};
      material.roughness = 1.0f;
      mesh.materials.push_back(std::move(material));
      current = &mesh.materials.back();
      continue;
    }
    if (current == nullptr) continue;
    if (keyword == "Kd") {
      const ir::Color c = rgb(line, current->base_color.a);
      current->base_color = c;
    } else if (keyword == "Ke") {
      const ir::Color c = rgb(line, 1.0f);
      current->emissive = {c.r, c.g, c.b};
    } else if (keyword == "d") {
      float d = 1;
      if (parse_float(next_token(line), d)) current->base_color.a = std::clamp(d, 0.0f, 1.0f);
    } else if (keyword == "Tr") {
      float t = 0;
      if (parse_float(next_token(line), t)) current->base_color.a = std::clamp(1.0f - t, 0.0f, 1.0f);
    } else if (keyword == "Ns") {
      float ns = 0;
      if (parse_float(next_token(line), ns) && ns >= 0) {
        current->roughness = std::clamp(std::sqrt(2.0f / (ns + 2.0f)), 0.0f, 1.0f);
      }
    } else if (keyword == "Pr") {
      float v = 1;
      if (parse_float(next_token(line), v)) current->roughness = std::clamp(v, 0.0f, 1.0f);
    } else if (keyword == "Pm") {
      float v = 0;
      if (parse_float(next_token(line), v)) current->metallic = std::clamp(v, 0.0f, 1.0f);
    } else if (keyword == "map_Kd") {
      current->base_color_texture = texture(line);
    } else if (keyword == "map_Bump" || keyword == "bump" || keyword == "norm" || keyword == "map_bump") {
      current->normal_texture = texture(line);
    }
  }
}

// Parses OBJ text into `mesh`; `obj_name` locates companion files.
Result<void> parse_obj(std::string_view text, std::string_view obj_name, ir::Mesh& mesh, ReadContext& ctx,
                       const FileLookup& lookup) {
  std::vector<ir::Vec3> v;
  std::vector<ir::Color> vc;
  std::vector<ir::Vec2> vt;
  std::vector<ir::Vec3> vn;
  std::unordered_map<ObjCorner, std::uint32_t, ObjCornerHash> corner_index;
  std::vector<ObjCorner> corners_of_vertex;
  std::vector<std::uint32_t> smoothing;  // per face
  std::uint32_t current_smoothing = 0;
  bool any_smoothing = false;
  bool any_vn = false, any_vt = false;
  std::vector<std::string> mtllibs;
  std::string current_group;
  std::string current_material;
  std::string current_object;
  bool all_triangles = true;
  std::size_t skipped_elements = 0;
  std::vector<ObjCorner> face;

  const auto open_group = [&]() {
    if (!mesh.groups.empty() && mesh.groups.back().face_count == 0) mesh.groups.pop_back();
    mesh.groups.push_back({current_group, current_material, mesh.face_count(), 0});
  };
  const auto open_object = [&](std::string name) {
    if (!mesh.objects.empty() && mesh.objects.back().face_count == 0) mesh.objects.pop_back();
    mesh.objects.push_back({std::move(name), mesh.face_count(), 0, {}});
  };

  const std::size_t total = text.size();
  LineReader lines(text);
  std::string_view raw;
  std::string joined;
  while (lines.next(raw)) {
    if ((lines.line_number() & 0x3FFF) == 0) {
      CVT_TRY(ctx.checkpoint(0.9 * static_cast<double>(lines.position()) / static_cast<double>(total + 1)));
    }
    std::string_view line = raw;
    if (!line.empty() && line.back() == '\\') {  // continuation lines
      joined.assign(line.substr(0, line.size() - 1));
      while (lines.next(raw)) {
        if (!raw.empty() && raw.back() == '\\') {
          joined.append(raw.substr(0, raw.size() - 1));
          joined.push_back(' ');
          continue;
        }
        joined.append(raw);
        break;
      }
      line = joined;
    }
    line = strip_comment(line);
    std::string_view keyword = next_token(line);
    if (keyword.empty()) continue;
    if (keyword == "v") {
      NumberScanner scan(line, false);
      double values[7] = {};
      int count = 0;
      while (count < 7 && scan.next(values[count])) ++count;
      if (count < 3) return fail(Status::Decode, "malformed vertex on line {}", lines.line_number());
      v.push_back({static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])});
      if (count >= 6) {
        const bool bytes = values[3] > 1.0 || values[4] > 1.0 || values[5] > 1.0;
        if (vc.size() + 1 < v.size()) vc.resize(v.size() - 1, ir::Color{});
        vc.push_back(color_from_srgb(values[3], values[4], values[5], -1.0, bytes ? 255.0 : 1.0));
      }
      if ((v.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx, v.size(), "vertices"));
    } else if (keyword == "vt") {
      NumberScanner scan(line, false);
      float u = 0, w = 0;
      if (!scan.next_float(u)) return fail(Status::Decode, "malformed texture coordinate on line {}", lines.line_number());
      if (!scan.next_float(w)) w = 0;
      vt.push_back({u, w});
    } else if (keyword == "vn") {
      NumberScanner scan(line, false);
      ir::Vec3 n;
      if (!scan.next_float(n.x) || !scan.next_float(n.y) || !scan.next_float(n.z)) {
        return fail(Status::Decode, "malformed normal on line {}", lines.line_number());
      }
      vn.push_back(n);
    } else if (keyword == "f" || keyword == "fo") {
      face.clear();
      for (std::string_view token = next_token(line); !token.empty(); token = next_token(line)) {
        ObjCorner corner;
        std::int64_t* fields[3] = {&corner.v, &corner.vt, &corner.vn};
        const std::size_t counts[3] = {v.size(), vt.size(), vn.size()};
        std::size_t field = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= token.size(); ++i) {
          if (i < token.size() && token[i] != '/') continue;
          if (field >= 3) return fail(Status::Decode, "malformed face on line {}", lines.line_number());
          const std::string_view part = token.substr(start, i - start);
          if (!part.empty()) {
            std::int64_t index = 0;
            if (!parse_int(part, index) || index == 0) {
              return fail(Status::Decode, "malformed face index '{}' on line {}", part, lines.line_number());
            }
            const auto count = static_cast<std::int64_t>(counts[field]);
            if (index < 0) index = count + 1 + index;  // relative to the end
            if (index < 1 || index > count) {
              return fail(Status::Decode, "face index {} out of range on line {}", part, lines.line_number());
            }
            *fields[field] = index;
          } else if (field == 0) {
            return fail(Status::Decode, "malformed face on line {}", lines.line_number());
          }
          ++field;
          start = i + 1;
        }
        face.push_back(corner);
      }
      if (face.size() < 3) {
        ++skipped_elements;
        continue;
      }
      if (mesh.groups.empty() && (!current_group.empty() || !current_material.empty())) open_group();
      for (const ObjCorner& corner : face) {
        const auto [it, inserted] = corner_index.emplace(corner, static_cast<std::uint32_t>(corners_of_vertex.size()));
        if (inserted) {
          corners_of_vertex.push_back(corner);
          if ((corners_of_vertex.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx, corners_of_vertex.size(), "vertices"));
          any_vt = any_vt || corner.vt != 0;
          any_vn = any_vn || corner.vn != 0;
        }
        mesh.indices.push_back(it->second);
      }
      if ((mesh.indices.size() & 0xFFFF) < face.size()) CVT_TRY(check_elements(ctx, mesh.indices.size(), "face corners"));
      mesh.face_sizes.push_back(static_cast<std::uint32_t>(face.size()));
      if (face.size() != 3) all_triangles = false;
      smoothing.push_back(current_smoothing);
      if (!mesh.groups.empty()) mesh.groups.back().face_count++;
      if (!mesh.objects.empty()) mesh.objects.back().face_count++;
    } else if (keyword == "g") {
      current_group = std::string(util::trim(line));
      open_group();
    } else if (keyword == "o") {
      current_group = std::string(util::trim(line));
      open_object(current_group);
      open_group();
    } else if (keyword == "usemtl") {
      current_material = std::string(util::trim(line));
      open_group();
    } else if (keyword == "s") {
      const std::string_view value = util::trim(line);
      std::int64_t group = 0;
      if (value == "off" || value.empty()) {
        current_smoothing = 0;
      } else if (parse_int(value, group) && group >= 0) {
        current_smoothing = static_cast<std::uint32_t>(std::min<std::int64_t>(group, 0x7FFFFFFF));
      } else {
        current_smoothing = 1;  // "s on"
      }
      any_smoothing = any_smoothing || current_smoothing != 0;
    } else if (keyword == "mtllib") {
      // Several names may follow; names with spaces are rare but legal for one file.
      std::vector<std::string_view> names = tokens(line);
      if (names.size() > 1 && util::iends_with(util::trim(line), ".mtl") &&
          std::any_of(names.begin(), names.end() - 1, [](std::string_view n) { return !util::iends_with(n, ".mtl"); })) {
        mtllibs.emplace_back(util::trim(line));
      } else {
        for (const std::string_view name : names) mtllibs.emplace_back(name);
      }
    } else if (keyword == "l" || keyword == "p" || keyword == "curv" || keyword == "surf") {
      ++skipped_elements;
    }
  }
  if (skipped_elements > 0) ctx.warn(std::format("{} points, lines or curves were not converted.", skipped_elements));
  if (!mesh.groups.empty() && mesh.groups.back().face_count == 0) mesh.groups.pop_back();
  if (!mesh.objects.empty() && mesh.objects.back().face_count == 0) mesh.objects.pop_back();
  // A single unnamed group carries no information.
  if (mesh.groups.size() == 1 && mesh.groups[0].name.empty() && mesh.groups[0].material.empty()) mesh.groups.clear();

  if (corners_of_vertex.empty()) {
    // Vertices only: a point cloud.
    mesh.positions = std::move(v);
    if (!vc.empty()) {
      vc.resize(mesh.positions.size(), ir::Color{});
      mesh.colors = std::move(vc);
    }
  } else {
    const std::size_t n = corners_of_vertex.size();
    mesh.positions.reserve(n);
    for (const ObjCorner& c : corners_of_vertex) mesh.positions.push_back(v[static_cast<std::size_t>(c.v - 1)]);
    if (!vc.empty()) {
      vc.resize(v.size(), ir::Color{});
      for (const ObjCorner& c : corners_of_vertex) mesh.colors.push_back(vc[static_cast<std::size_t>(c.v - 1)]);
    }
    if (any_vt) {
      for (const ObjCorner& c : corners_of_vertex) {
        mesh.texcoords.push_back(c.vt > 0 ? vt[static_cast<std::size_t>(c.vt - 1)] : ir::Vec2{});
      }
    }
    if (any_vn) {
      for (const ObjCorner& c : corners_of_vertex) {
        mesh.normals.push_back(c.vn > 0 ? vn[static_cast<std::size_t>(c.vn - 1)] : ir::Vec3{});
      }
    }
  }
  if (all_triangles) mesh.face_sizes.clear();

  // Materials.
  for (const std::string& lib : mtllibs) {
    const std::string path = resolve_relative(obj_name, lib);
    std::optional<Bytes> data;
    if (lookup) {
      data = lookup(path);
      if (!data) data = lookup(base_name(lib));
    }
    if (!data) {
      ctx.warn(std::format("Material library '{}' was not supplied; material names were kept without their colours "
                           "and textures (convert a ZIP holding the model, its .mtl and textures to keep them).",
                           lib));
      continue;
    }
    parse_mtl(as_chars(*data), path, mesh, lookup, ctx);
  }

  // OBJ declares no axes, but the de-facto convention (Blender, Maya, 3ds Max
  // exporters, most viewers) is +Y up.
  mesh.source_up_axis = "Y";
  CVT_TRY(finish_read(mesh, ctx));
  // Normals: smoothing groups decide when the file has none.
  if (mesh.normals.empty() && any_smoothing && !mesh.point_cloud) {
    generate_normals_by_groups(mesh, smoothing, false);
  }
  return {};
}

Result<AnyIR> read_obj(ReadContext& ctx) {
  ir::Mesh mesh;
  CVT_TRY(parse_obj(as_chars(ctx.input), ctx.filename, mesh, ctx, {}));
  return mesh;
}

Result<AnyIR> read_obj_zip(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  std::optional<std::size_t> model;
  for (std::size_t i = 0; i < zip.entries().size(); ++i) {
    const util::ZipEntry& entry = zip.entries()[i];
    if (!entry.is_directory && util::iends_with(entry.name, ".obj") && !entry.name.starts_with("__MACOSX/")) {
      model = i;
      break;
    }
  }
  if (!model) return fail(Status::Decode, "the archive contains no .obj model");
  const std::string obj_name = zip.entries()[*model].name;
  CVT_TRY_ASSIGN(const Bytes text, zip.read(*model));
  // Case-insensitive lookup of companion files by path, then by base name.
  std::map<std::string, std::size_t> by_path, by_base;
  for (std::size_t i = 0; i < zip.entries().size(); ++i) {
    const auto safe = zip.entries()[i].safe_path();
    if (!safe) continue;
    by_path.emplace(util::to_lower(*safe), i);
    by_base.emplace(util::to_lower(base_name(*safe)), i);
  }
  const FileLookup lookup = [&](std::string_view name) -> std::optional<Bytes> {
    const std::string key = util::to_lower(resolve_relative("", name));
    auto it = by_path.find(key);
    if (it == by_path.end()) {
      it = by_base.find(util::to_lower(base_name(name)));
      if (it == by_base.end()) return std::nullopt;
    }
    auto data = zip.read(it->second);
    if (!data) return std::nullopt;
    return std::move(*data);
  };
  ir::Mesh mesh;
  CVT_TRY(parse_obj(as_chars(text), obj_name, mesh, ctx, lookup));
  return mesh;
}

// --------------------------------------------------------------------------- writing

constexpr WriterTraits obj_traits() {
  WriterTraits traits;
  traits.text = true;
  traits.point_clouds = true;
  traits.up = WriterTraits::Up::Y;  // the de-facto OBJ convention
  return traits;
}

std::string material_name(const std::string& name, std::size_t index) {
  std::string out;
  for (const char c : name) out.push_back(util::is_ascii_space(c) || c == '#' ? '_' : c);
  return out.empty() ? std::format("material_{}", index) : out;
}

Result<std::string> obj_text(const ir::Mesh& mesh, WriteContext& ctx, std::string_view mtllib) {
  const int precision = write_precision(ctx);
  std::string out;
  out.reserve(mesh.positions.size() * 64 + mesh.indices.size() * 12 + 256);
  if (!options::resolved_strip_metadata(ctx.options)) {
    for (const auto& [key, value] : mesh.metadata) {
      std::string clean;
      for (const char c : value) clean.push_back(c == '\n' || c == '\r' ? ' ' : c);
      out += std::format("# {}: {}\n", key, clean);
    }
  }
  if (!mtllib.empty()) out += std::format("mtllib {}\n", mtllib);
  const bool colors = !mesh.colors.empty();
  for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
    if ((i & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(0.8 + 0.1 * static_cast<double>(i) / static_cast<double>(mesh.positions.size())));
    const ir::Vec3& p = mesh.positions[i];
    out += "v ";
    append_number(out, p.x, precision);
    out += ' ';
    append_number(out, p.y, precision);
    out += ' ';
    append_number(out, p.z, precision);
    if (colors) {
      const ir::Color& c = mesh.colors[i];
      for (const float component : {linear_to_srgb(c.r), linear_to_srgb(c.g), linear_to_srgb(c.b)}) {
        out += ' ';
        append_number(out, component, precision == 0 ? 6 : precision);
      }
    }
    out += '\n';
  }
  for (const ir::Vec2& t : mesh.texcoords) {
    out += "vt ";
    append_number(out, t.x, precision);
    out += ' ';
    append_number(out, t.y, precision);
    out += '\n';
  }
  for (const ir::Vec3& n : mesh.normals) {
    out += "vn ";
    append_number(out, n.x, precision == 0 ? 7 : precision);
    out += ' ';
    append_number(out, n.y, precision == 0 ? 7 : precision);
    out += ' ';
    append_number(out, n.z, precision == 0 ? 7 : precision);
    out += '\n';
  }
  if (mesh.point_cloud) return out;

  // Per face: which object starts, which group starts.
  const std::size_t faces = mesh.face_count();
  std::vector<const ir::MeshObject*> object_at(faces + 1, nullptr);
  for (const ir::MeshObject& object : mesh.objects) {
    if (object.first_face < faces) object_at[object.first_face] = &object;
  }
  std::vector<const ir::MeshGroup*> group_at(faces + 1, nullptr);
  for (const ir::MeshGroup& group : mesh.groups) {
    if (group.first_face < faces) group_at[group.first_face] = &group;
  }
  std::map<std::string, std::size_t> material_index;
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) material_index.emplace(mesh.materials[i].name, i);
  const bool vt = !mesh.texcoords.empty();
  const bool vn = !mesh.normals.empty();
  const std::vector<std::size_t> offsets = face_offsets(mesh);
  std::string current_material;
  for (std::size_t f = 0; f < faces; ++f) {
    if (object_at[f] != nullptr) out += std::format("o {}\n", object_at[f]->name.empty() ? "object" : object_at[f]->name);
    if (group_at[f] != nullptr) {
      const ir::MeshGroup& group = *group_at[f];
      if (!group.name.empty() && (object_at[f] == nullptr || object_at[f]->name != group.name)) {
        out += std::format("g {}\n", group.name);
      }
      if (group.material != current_material) {
        current_material = group.material;
        const auto it = material_index.find(group.material);
        out += std::format("usemtl {}\n", current_material.empty()
                                              ? std::string("default")
                                              : material_name(current_material, it == material_index.end() ? 0 : it->second));
      }
    }
    out += 'f';
    for (std::size_t c = offsets[f]; c < offsets[f + 1]; ++c) {
      const std::uint32_t index = mesh.indices[c] + 1;
      out += ' ';
      out += std::to_string(index);
      if (vt || vn) {
        out += '/';
        if (vt) out += std::to_string(index);
        if (vn) {
          out += '/';
          out += std::to_string(index);
        }
      }
    }
    out += '\n';
  }
  return out;
}

std::string mtl_text(const ir::Mesh& mesh, const std::vector<std::string>& texture_files) {
  std::string out;
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
    const ir::Material& m = mesh.materials[i];
    out += std::format("newmtl {}\n", material_name(m.name, i));
    out += "Kd ";
    append_number(out, m.base_color.r, 6);
    out += ' ';
    append_number(out, m.base_color.g, 6);
    out += ' ';
    append_number(out, m.base_color.b, 6);
    out += "\nKa 0 0 0\nKs 0.5 0.5 0.5\n";
    const float r = std::clamp(m.roughness, 0.02f, 1.0f);
    out += "Ns ";
    append_number(out, std::clamp(2.0f / (r * r) - 2.0f, 0.0f, 1000.0f), 6);
    out += "\nd ";
    append_number(out, m.base_color.a, 6);
    out += "\nillum 2\nPr ";
    append_number(out, m.roughness, 6);
    out += "\nPm ";
    append_number(out, m.metallic, 6);
    out += '\n';
    if (m.emissive.x > 0 || m.emissive.y > 0 || m.emissive.z > 0) {
      out += "Ke ";
      append_number(out, m.emissive.x, 6);
      out += ' ';
      append_number(out, m.emissive.y, 6);
      out += ' ';
      append_number(out, m.emissive.z, 6);
      out += '\n';
    }
    const auto map = [&](std::string_view key, int texture) {
      if (texture >= 0 && static_cast<std::size_t>(texture) < texture_files.size() && !texture_files[texture].empty()) {
        out += std::format("{} {}\n", key, texture_files[texture]);
      }
    };
    map("map_Kd", m.base_color_texture);
    map("norm", m.normal_texture);
    out += '\n';
  }
  return out;
}

Result<Bytes> write_obj(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, obj_traits()));
  if (!mesh.materials.empty()) {
    ctx.warn("Material colours and textures need a separate .mtl file; only the material names were written "
             "(choose the OBJ (ZIP) target to keep them).");
  }
  CVT_TRY_ASSIGN(const std::string text, obj_text(mesh, ctx, {}));
  CVT_TRY(ctx.limits.check_output(text.size()));
  return to_bytes(text);
}

Result<Bytes> write_obj_zip(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, obj_traits()));
  std::vector<std::string> texture_files(mesh.textures.size());
  util::ZipWriter zip;
  const bool materials = !mesh.materials.empty();
  CVT_TRY_ASSIGN(const std::string text, obj_text(mesh, ctx, materials ? "model.mtl" : ""));
  CVT_TRY(zip.add("model.obj", text));
  std::map<std::string, int> used_names;
  for (std::size_t i = 0; i < mesh.textures.size(); ++i) {
    const ir::Texture& texture = mesh.textures[i];
    if (texture.data.empty()) {
      ctx.warn(std::format("Texture '{}' has no image data and was left out.", texture.uri));
      continue;
    }
    std::string stem;
    for (const char c : std::string(base_name(texture.name.empty() ? texture.uri : texture.name))) {
      stem.push_back(util::is_ascii_space(c) || c == '#' || c == '/' || c == '\\' ? '_' : c);
    }
    if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
    if (stem.empty()) stem = std::format("texture_{}", i);
    if (int& count = used_names[stem]; count++ > 0) stem += std::format("_{}", count);
    const std::string mime = texture.mime.empty() ? image_mime(texture.data, texture.uri) : texture.mime;
    texture_files[i] = std::format("textures/{}.{}", stem, image_extension(mime));
    util::ZipEntryOptions stored;
    stored.level = 0;  // images are compressed already
    CVT_TRY(zip.add(texture_files[i], texture.data, stored));
  }
  if (materials) CVT_TRY(zip.add("model.mtl", mtl_text(mesh, texture_files)));
  CVT_TRY_ASSIGN(Bytes out, zip.finish());
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

// --------------------------------------------------------------------------- detection

int sniff_obj(ByteView data, std::string_view filename) {
  if (!looks_textual(data)) return 0;
  LineReader lines(text_head(data, 8192));
  std::string_view line;
  int v = 0, f = 0, other_obj = 0, foreign = 0;
  while (lines.next(line) && lines.line_number() < 200) {
    line = util::trim(line);
    if (line.empty() || line.front() == '#') continue;
    std::string_view rest = line;
    const std::string_view keyword = next_token(rest);
    if (keyword == "v") {
      NumberScanner scan(rest, false);
      double x = 0;
      int numbers = 0;
      while (scan.next(x)) ++numbers;
      if (numbers >= 3 && !scan.bad()) ++v;
      else ++foreign;
    } else if (keyword == "f") {
      ++f;
    } else if (keyword == "vt" || keyword == "vn" || keyword == "g" || keyword == "o" || keyword == "usemtl" ||
               keyword == "mtllib" || keyword == "s") {
      ++other_obj;
    } else {
      ++foreign;
    }
  }
  if (v >= 3 && foreign <= (v + f + other_obj) / 20) return f > 0 || other_obj > 0 ? 88 : 70;
  if (v >= 1 && has_extension(filename, "obj") && foreign == 0) return 60;
  return 0;
}

int sniff_obj_zip(ByteView data, std::string_view /*filename*/) {
  if (data.size() < 30 || !as_chars(data).starts_with("PK")) return 0;
  Limits limits;
  limits.max_archive_entries = 10000;
  auto zip = util::ZipReader::open(data, limits);
  if (!zip) return 0;
  bool obj = false;
  bool office = false;
  for (const util::ZipEntry& entry : zip->entries()) {
    if (util::iends_with(entry.name, ".obj") && !entry.is_directory) obj = true;
    if (entry.name == "[Content_Types].xml" || entry.name == "mimetype") office = true;
  }
  return obj && !office ? 93 : 0;
}

}  // namespace

void register_obj(Registry& registry) {
  FormatInfo info;
  info.id = "obj";
  info.name = "Wavefront OBJ model";
  info.description = "Widely supported text format for polygon meshes with texture coordinates and normals.";
  info.category = Category::Model3D;
  info.extensions = {"obj"};
  info.mime = "model/obj";
  info.popularity = 75;
  info.sniff = sniff_obj;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "obj";
  codec.kind = Kind::Mesh;
  codec.read = read_obj;
  codec.write = write_obj;
  codec.write_options = mesh_write_options(obj_traits());
  registry.add_codec(std::move(codec));

  FormatInfo zipped;
  zipped.id = "obj.zip";
  zipped.name = "Wavefront OBJ model with materials (ZIP)";
  zipped.description = "An OBJ model packed together with its .mtl materials and texture images.";
  zipped.category = Category::Model3D;
  zipped.extensions = {"obj.zip"};
  zipped.mime = "application/zip";
  zipped.popularity = 30;
  zipped.sniff = sniff_obj_zip;
  registry.add_format(std::move(zipped));

  Codec zip_codec;
  zip_codec.format_id = "obj.zip";
  zip_codec.kind = Kind::Mesh;
  zip_codec.read = read_obj_zip;
  zip_codec.write = write_obj_zip;
  zip_codec.write_options = mesh_write_options(obj_traits());
  registry.add_codec(std::move(zip_codec));
}

}  // namespace cvt::mesh
