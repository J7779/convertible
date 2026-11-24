// Convertible core - mesh family: 3D Studio (.3ds) read and write.
//
// A tree of chunks (16-bit id, 32-bit length including the 6-byte header).
// Objects (0x4000) hold triangle meshes in world coordinates with face
// material groups, smoothing-group bit masks and texture coordinates;
// materials (0xAFFF) hold colours, shininess, transparency and texture file
// names. Counts are 16-bit, so the writer splits large meshes.
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
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

constexpr std::uint16_t chunk_main = 0x4D4D, chunk_version = 0x0002, chunk_editor = 0x3D3D, chunk_mesh_version = 0x3D3E,
                        chunk_scale = 0x0100, chunk_object = 0x4000, chunk_trimesh = 0x4100, chunk_vertices = 0x4110,
                        chunk_faces = 0x4120, chunk_face_material = 0x4130, chunk_uv = 0x4140, chunk_smoothing = 0x4150,
                        chunk_matrix = 0x4160, chunk_material = 0xAFFF, chunk_material_name = 0xA000,
                        chunk_ambient = 0xA010, chunk_diffuse = 0xA020, chunk_specular = 0xA030, chunk_shininess = 0xA040,
                        chunk_transparency = 0xA050, chunk_texture = 0xA200, chunk_map_file = 0xA300,
                        chunk_color_f = 0x0010, chunk_color_24 = 0x0011, chunk_color_24_gamma = 0x0012,
                        chunk_color_f_gamma = 0x0013, chunk_percent_i = 0x0030, chunk_percent_f = 0x0031;

struct Chunk {
  std::uint16_t id = 0;
  ByteView body;  // without the header
};

// Children of a chunk body (starting at `offset`), bounds-checked.
Result<std::vector<Chunk>> children(ByteView body, std::size_t offset = 0) {
  std::vector<Chunk> out;
  util::ByteReader r(body);
  if (!r.seek(offset)) return fail(Status::Decode, "a 3DS chunk is truncated");
  while (r.remaining() >= 6) {
    const std::uint16_t id = r.u16le();
    const std::uint32_t length = r.u32le();
    if (length < 6 || length - 6 > r.remaining()) return fail(Status::Decode, "a 3DS chunk (0x{:04X}) overruns its parent", id);
    out.push_back({id, r.bytes(length - 6)});
    if (out.size() > 1'000'000) return fail(Status::Limit, "the 3DS file has too many chunks");
  }
  return out;
}

std::string cstring(util::ByteReader& r) {
  std::string out(r.cstring());
  return util::sanitize_utf8(out);
}

// Colour from a colour-holding chunk (0x0010-0x0013 children).
ir::Color read_color(ByteView body) {
  auto list = children(body);
  if (!list) return {};
  ir::Color linear{0.8f, 0.8f, 0.8f, 1};
  bool have = false;
  for (const Chunk& c : *list) {
    util::ByteReader r(c.body);
    if (c.id == chunk_color_24 || (c.id == chunk_color_24_gamma && !have)) {
      const double red = r.u8(), green = r.u8(), blue = r.u8();
      if (r.ok()) linear = color_from_srgb(red, green, blue), have = true;
    } else if (c.id == chunk_color_f || (c.id == chunk_color_f_gamma && !have)) {
      const float red = r.f32le(), green = r.f32le(), blue = r.f32le();
      if (r.ok()) linear = {red, green, blue, 1}, have = true;
    }
  }
  return linear;
}

double read_percent(ByteView body) {
  auto list = children(body);
  if (!list) return 0;
  for (const Chunk& c : *list) {
    util::ByteReader r(c.body);
    if (c.id == chunk_percent_i) return r.u16le() / 100.0;
    if (c.id == chunk_percent_f) return r.f32le() / 100.0;
  }
  return 0;
}

Result<void> read_material(ByteView body, ir::Mesh& mesh) {
  CVT_TRY_ASSIGN(const std::vector<Chunk> list, children(body));
  ir::Material m;
  m.base_color = {0.8f, 0.8f, 0.8f, 1};
  for (const Chunk& c : list) {
    util::ByteReader r(c.body);
    switch (c.id) {
      case chunk_material_name: m.name = cstring(r); break;
      case chunk_diffuse: m.base_color = read_color(c.body); break;
      case chunk_shininess: m.roughness = static_cast<float>(std::clamp(1.0 - read_percent(c.body), 0.0, 1.0)); break;
      case chunk_transparency: m.base_color.a = static_cast<float>(std::clamp(1.0 - read_percent(c.body), 0.0, 1.0)); break;
      case chunk_texture: {
        CVT_TRY_ASSIGN(const std::vector<Chunk> maps, children(c.body));
        for (const Chunk& map : maps) {
          if (map.id != chunk_map_file) continue;
          util::ByteReader mr(map.body);
          ir::Texture texture;
          texture.uri = cstring(mr);
          texture.name = std::string(base_name(texture.uri));
          m.base_color_texture = static_cast<int>(mesh.textures.size());
          mesh.textures.push_back(std::move(texture));
        }
        break;
      }
      default: break;
    }
  }
  if (m.name.empty()) m.name = std::format("material_{}", mesh.materials.size());
  mesh.materials.push_back(std::move(m));
  return {};
}

Result<void> read_object(ByteView body, ir::Mesh& mesh, ReadContext& ctx) {
  util::ByteReader r(body);
  const std::string name = cstring(r);
  if (!r.ok()) return fail(Status::Decode, "a 3DS object name is truncated");
  CVT_TRY_ASSIGN(const std::vector<Chunk> list, children(body, r.position()));
  for (const Chunk& c : list) {
    if (c.id != chunk_trimesh) continue;
    CVT_TRY_ASSIGN(const std::vector<Chunk> parts, children(c.body));
    ir::Mesh part;
    std::vector<std::uint32_t> smoothing;
    std::vector<std::string> face_material;
    ir::Mat4 matrix;
    for (const Chunk& p : parts) {
      util::ByteReader pr(p.body);
      if (p.id == chunk_vertices) {
        const std::uint16_t count = pr.u16le();
        CVT_TRY(check_records(ctx, count, 12, pr.remaining(), "vertices"));
        for (std::uint16_t i = 0; i < count; ++i) {
          const float x = pr.f32le(), y = pr.f32le(), z = pr.f32le();
          part.positions.push_back({x, y, z});
        }
      } else if (p.id == chunk_uv) {
        const std::uint16_t count = pr.u16le();
        CVT_TRY(check_records(ctx, count, 8, pr.remaining(), "texture coordinates"));
        for (std::uint16_t i = 0; i < count; ++i) {
          const float u = pr.f32le(), v = pr.f32le();
          part.texcoords.push_back({u, v});
        }
      } else if (p.id == chunk_matrix) {
        float v[12];
        for (float& x : v) x = pr.f32le();
        if (pr.ok()) {
          for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 3; ++row) matrix.m[col * 4 + row] = v[col * 3 + row];
          }
        }
      } else if (p.id == chunk_faces) {
        const std::uint16_t count = pr.u16le();
        CVT_TRY(check_records(ctx, count, 8, pr.remaining(), "faces"));
        for (std::uint16_t i = 0; i < count; ++i) {
          const std::uint16_t a = pr.u16le(), b = pr.u16le(), cc = pr.u16le();
          pr.skip(2);  // edge visibility flags
          part.indices.insert(part.indices.end(), {a, b, cc});
        }
        smoothing.assign(count, 0);
        face_material.assign(count, {});
        CVT_TRY_ASSIGN(const std::vector<Chunk> extras, children(p.body, pr.position()));
        for (const Chunk& e : extras) {
          util::ByteReader er(e.body);
          if (e.id == chunk_face_material) {
            const std::string material = cstring(er);
            const std::uint16_t n = er.u16le();
            for (std::uint16_t k = 0; k < n && er.ok(); ++k) {
              const std::uint16_t face = er.u16le();
              if (er.ok() && face < count) face_material[face] = material;
            }
          } else if (e.id == chunk_smoothing) {
            for (std::uint16_t k = 0; k < count && er.remaining() >= 4; ++k) smoothing[k] = er.u32le();
          }
        }
      }
      if (!pr.ok()) return fail(Status::Decode, "a 3DS mesh chunk is truncated");
    }
    if (part.indices.empty()) continue;
    for (const std::uint32_t index : part.indices) {
      if (index >= part.positions.size()) return fail(Status::Decode, "a 3DS face refers to missing vertex {}", index);
    }
    if (part.texcoords.size() != part.positions.size()) part.texcoords.clear();
    for (std::size_t f = 0; f < face_material.size(); ++f) {
      if (part.groups.empty() || part.groups.back().material != face_material[f]) part.groups.push_back({name, face_material[f], f, 0});
      part.groups.back().face_count++;
    }
    generate_normals_by_groups(part, smoothing, true);
    const std::size_t first = mesh.face_count();
    const auto materials = std::move(mesh.materials);
    const auto textures = std::move(mesh.textures);
    append(mesh, part);
    mesh.materials = materials;
    mesh.textures = textures;
    mesh.objects.push_back({name, first, mesh.face_count() - first, matrix});
    CVT_TRY(check_elements(ctx, mesh.positions.size(), "vertices"));
  }
  return {};
}

Result<AnyIR> read_3ds(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u16le() != chunk_main) return fail(Status::Decode, "not a 3DS file");
  const std::uint32_t length = r.u32le();
  if (length < 6 || length > ctx.input.size()) return fail(Status::Decode, "the 3DS file is truncated");
  CVT_TRY_ASSIGN(const std::vector<Chunk> top, children(ctx.input.subspan(6, length - 6)));
  ir::Mesh mesh;
  for (const Chunk& c : top) {
    if (c.id != chunk_editor) continue;
    CVT_TRY_ASSIGN(const std::vector<Chunk> editor, children(c.body));
    for (const Chunk& e : editor) {
      if (e.id == chunk_material) CVT_TRY(read_material(e.body, mesh));
    }
    std::size_t done = 0;
    for (const Chunk& e : editor) {
      if (e.id != chunk_object) continue;
      CVT_TRY(ctx.checkpoint(0.9 * static_cast<double>(++done) / (static_cast<double>(editor.size()) + 1.0)));
      CVT_TRY(read_object(e.body, mesh, ctx));
    }
  }
  if (mesh.positions.empty()) return fail(Status::Decode, "the 3DS file contains no meshes");
  // 3DS stores vertices in world space; the object matrices are informational.
  if (mesh.objects.size() == 1) mesh.objects.clear();
  for (const ir::Texture& t : mesh.textures) {
    ctx.warn(std::format("Texture '{}' is a separate file and was not included.", t.name));
  }
  mesh.source_up_axis = "Z";
  convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- writing

constexpr WriterTraits max3ds_traits() {
  WriterTraits traits;
  traits.triangles_only = true;
  traits.normals = false;
  traits.colors = false;
  traits.objects = true;
  traits.up = WriterTraits::Up::Z;
  return traits;
}

class ChunkWriter {
 public:
  void begin(std::uint16_t id) {
    out_.u16le(id);
    starts_.push_back(out_.size());
    out_.u32le(0);
  }
  void end() {
    const std::size_t start = starts_.back();
    starts_.pop_back();
    out_.patch_u32le(start, static_cast<std::uint32_t>(out_.size() - start + 2));
  }
  util::ByteWriter& out() { return out_; }
  void text(std::string_view s) {
    out_.text(s);
    out_.u8(0);
  }

 private:
  util::ByteWriter out_;
  std::vector<std::size_t> starts_;
};

std::string short_name(std::string_view name, std::size_t limit, std::map<std::string, int>& used, std::string_view fallback) {
  std::string base;
  for (const char c : name) {
    if (base.size() >= limit) break;
    base.push_back(static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 127 ? c : '_');
  }
  if (base.empty()) base = std::string(fallback);
  std::string out = base;
  for (int k = 1; used.contains(out); ++k) {
    const std::string suffix = std::to_string(k);
    out = base.substr(0, std::min(base.size(), limit - suffix.size())) + suffix;
  }
  used[out] = 1;
  return out;
}

Result<Bytes> write_3ds(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Mesh mesh, prepare_for_write(value.get<ir::Mesh>(), ctx, max3ds_traits()));
  std::map<std::string, int> used_materials, used_objects;
  std::map<std::string, std::string> material_name;
  ChunkWriter w;
  w.begin(chunk_main);
  w.begin(chunk_version);
  w.out().u32le(3);
  w.end();
  w.begin(chunk_editor);
  w.begin(chunk_mesh_version);
  w.out().u32le(3);
  w.end();
  w.begin(chunk_scale);
  w.out().f32le(1.0f);
  w.end();
  for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
    const ir::Material& m = mesh.materials[i];
    const std::string name = short_name(m.name, 16, used_materials, "material");
    material_name[m.name] = name;
    w.begin(chunk_material);
    w.begin(chunk_material_name);
    w.text(name);
    w.end();
    const auto color = [&](std::uint16_t id, const ir::Color& c) {
      const auto b = color_to_srgb8(c);
      w.begin(id);
      w.begin(chunk_color_24);
      w.out().u8(b[0]).u8(b[1]).u8(b[2]);
      w.end();
      w.end();
    };
    color(chunk_ambient, {0, 0, 0, 1});
    color(chunk_diffuse, m.base_color);
    color(chunk_specular, {0.5f, 0.5f, 0.5f, 1});
    const auto percent = [&](std::uint16_t id, double value) {
      w.begin(id);
      w.begin(chunk_percent_i);
      w.out().u16le(static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0, 1.0) * 100)));
      w.end();
      w.end();
    };
    percent(chunk_shininess, 1.0 - m.roughness);
    percent(chunk_transparency, 1.0 - m.base_color.a);
    if (m.base_color_texture >= 0) {
      const ir::Texture& t = mesh.textures[static_cast<std::size_t>(m.base_color_texture)];
      std::string file(base_name(t.uri.empty() ? t.name : t.uri));
      if (file.empty() || file.size() > 12) file = std::format("tex{}.{}", m.base_color_texture, image_extension(t.mime.empty() ? "image/png" : t.mime));
      w.begin(chunk_texture);
      w.begin(chunk_percent_i);  // map strength 100%
      w.out().u16le(100);
      w.end();
      w.begin(chunk_map_file);
      w.text(file);
      w.end();
      w.end();
      ctx.warn(std::format("3DS refers to texture files by name; '{}' was not embedded.", file));
    }
    w.end();
  }
  const std::vector<ObjectPart> parts = split_objects(mesh, false);
  std::size_t written = 0;
  for (const ObjectPart& part : parts) {
    const ir::Mesh& m = part.mesh;
    const std::size_t faces = m.indices.size() / 3;
    std::vector<std::string> face_material(faces);
    for (const ir::MeshGroup& g : m.groups) {
      for (std::size_t f = g.first_face; f < g.first_face + g.face_count && f < faces; ++f) face_material[f] = g.material;
    }
    // Pieces of at most 65535 vertices and faces.
    std::size_t f = 0;
    while (f < faces) {
      CVT_TRY(ctx.checkpoint(0.8 + 0.2 * static_cast<double>(written) / static_cast<double>(mesh.indices.size() / 3 + 1)));
      std::unordered_map<std::uint32_t, std::uint16_t> local;
      std::vector<std::uint32_t> source;
      std::vector<std::array<std::uint16_t, 3>> tris;
      std::vector<std::size_t> origin;
      while (f < faces && tris.size() < 65535) {
        std::size_t added = 0;
        for (int k = 0; k < 3; ++k) added += local.contains(m.indices[f * 3 + k]) ? 0 : 1;
        if (source.size() + added > 65535) break;
        std::array<std::uint16_t, 3> t{};
        for (int k = 0; k < 3; ++k) {
          const std::uint32_t v = m.indices[f * 3 + k];
          auto [it, inserted] = local.emplace(v, static_cast<std::uint16_t>(source.size()));
          if (inserted) source.push_back(v);
          t[static_cast<std::size_t>(k)] = it->second;
        }
        tris.push_back(t);
        origin.push_back(f);
        ++f;
      }
      written += tris.size();
      w.begin(chunk_object);
      w.text(short_name(part.name, 10, used_objects, "object"));
      w.begin(chunk_trimesh);
      w.begin(chunk_vertices);
      w.out().u16le(static_cast<std::uint16_t>(source.size()));
      for (const std::uint32_t v : source) w.out().f32le(m.positions[v].x).f32le(m.positions[v].y).f32le(m.positions[v].z);
      w.end();
      if (!m.texcoords.empty()) {
        w.begin(chunk_uv);
        w.out().u16le(static_cast<std::uint16_t>(source.size()));
        for (const std::uint32_t v : source) w.out().f32le(m.texcoords[v].x).f32le(m.texcoords[v].y);
        w.end();
      }
      w.begin(chunk_matrix);
      for (const float v : {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f}) w.out().f32le(v);
      w.end();
      w.begin(chunk_faces);
      w.out().u16le(static_cast<std::uint16_t>(tris.size()));
      for (const auto& t : tris) w.out().u16le(t[0]).u16le(t[1]).u16le(t[2]).u16le(7);
      std::map<std::string, std::vector<std::uint16_t>> by_material;
      for (std::size_t k = 0; k < tris.size(); ++k) {
        const auto it = material_name.find(face_material[origin[k]]);
        if (it != material_name.end()) by_material[it->second].push_back(static_cast<std::uint16_t>(k));
      }
      for (const auto& [name, list] : by_material) {
        w.begin(chunk_face_material);
        w.text(name);
        w.out().u16le(static_cast<std::uint16_t>(list.size()));
        for (const std::uint16_t face : list) w.out().u16le(face);
        w.end();
      }
      w.begin(chunk_smoothing);
      for (std::size_t k = 0; k < tris.size(); ++k) w.out().u32le(1);
      w.end();
      w.end();  // faces
      w.end();  // trimesh
      w.end();  // object
    }
  }
  w.end();  // editor
  w.end();  // main
  Bytes out = w.out().take();
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

}  // namespace

void register_3ds(Registry& registry) {
  FormatInfo info;
  info.id = "3ds";
  info.name = "3D Studio model";
  info.description = "Legacy Autodesk 3D Studio mesh format.";
  info.category = Category::Model3D;
  info.extensions = {"3ds"};
  info.mime = "application/x-3ds";
  info.popularity = 30;
  info.sniff = [](ByteView data, std::string_view filename) -> int {
    util::ByteReader r(data);
    if (r.u16le() != chunk_main) return 0;
    const std::uint32_t length = r.u32le();
    const std::uint16_t first = r.u16le();
    if (!r.ok() || length < 12 || length > data.size() + 16) return 0;
    if (first == chunk_version || first == chunk_editor) return length == data.size() ? 92 : 75;
    return has_extension(filename, "3ds") ? 60 : 0;
  };
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = "3ds";
  codec.kind = Kind::Mesh;
  codec.read = read_3ds;
  codec.write = write_3ds;
  codec.write_options = mesh_write_options(max3ds_traits());
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::mesh
