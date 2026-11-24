// Convertible core - mesh family: read-only legacy and game formats.
//
//   lwo   LightWave object (LWO2): layers, points, polygons, surfaces,
//         texture and colour vertex maps (per-vertex and per-polygon)
//   md2   Quake II model, first frame
//   md3   Quake III model, first frame of every surface
//   ms3d  MilkShape 3D model: groups, materials, per-corner normals and uvs
//   x     DirectX text (.x) files: frames with matrices, meshes, normals,
//         texture coordinates, vertex colours and materials
//   vox   MagicaVoxel models, turned into coloured meshes by greedy meshing
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "families/mesh/common.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

struct CornerKey {
  std::uint32_t a = 0, b = 0, c = 0;
  friend bool operator==(const CornerKey&, const CornerKey&) = default;
};
struct CornerKeyHash {
  std::size_t operator()(const CornerKey& k) const noexcept {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(k.a) * 0x9E3779B97F4A7C15ull) ^ (static_cast<std::uint64_t>(k.b) << 21) ^
                                    (static_cast<std::uint64_t>(k.c) * 0xC2B2AE3D27D4EB4Full));
  }
};

std::string fixed_string(util::ByteReader& r, std::size_t size) {
  std::string_view raw = r.chars(size);
  const std::size_t nul = raw.find('\0');
  if (nul != std::string_view::npos) raw = raw.substr(0, nul);
  return util::sanitize_utf8(raw);
}

// --------------------------------------------------------------------------- LightWave LWO2

struct LwoLayer {
  std::string name;
  ir::Vec3 pivot{0, 0, 0};
  std::vector<ir::Vec3> points;
  std::vector<std::vector<std::uint32_t>> polygons;
  std::vector<std::uint32_t> polygon_tag;
  std::map<std::uint32_t, ir::Vec2> uv;              // per point
  std::map<std::pair<std::uint32_t, std::uint32_t>, ir::Vec2> uv_poly;  // (poly, point)
  std::map<std::uint32_t, ir::Color> color;
};

std::uint32_t lwo_vx(util::ByteReader& r) {
  if (r.peek_u8() == 0xFF) return r.u32be() & 0x00FFFFFFu;
  return r.u16be();
}

std::string lwo_string(util::ByteReader& r) {
  std::string out(r.cstring());
  if ((out.size() + 1) % 2 != 0) r.skip(1);  // padded to even length
  return util::sanitize_utf8(out);
}

Result<AnyIR> read_lwo(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("FORM")) return fail(Status::Decode, "not a LightWave object");
  const std::uint32_t form_size = r.u32be();
  const std::string_view kind = r.chars(4);
  if (kind == "LWOB" || kind == "LWLO") return fail(Status::Decode, "LightWave LWOB objects (version 5) are not supported; save as LWO2");
  if (kind != "LWO2") return fail(Status::Decode, "not a LightWave LWO2 object");
  const std::size_t end = std::min<std::size_t>(ctx.input.size(), std::size_t{form_size} + 8);
  std::vector<std::string> tags;
  std::vector<LwoLayer> layers;
  std::map<std::string, ir::Material> surfaces;
  const auto layer = [&]() -> LwoLayer& {
    if (layers.empty()) layers.emplace_back();
    return layers.back();
  };
  while (r.position() + 8 <= end) {
    const std::string_view id = r.chars(4);
    const std::uint32_t size = r.u32be();
    if (size > end - r.position()) return fail(Status::Decode, "a LightWave chunk overruns the file");
    util::ByteReader c(r.bytes(size));
    if (size % 2 != 0) r.skip(1);
    if (id == "TAGS") {
      while (!c.at_end()) {
        tags.push_back(lwo_string(c));
        if (!c.ok()) break;
      }
    } else if (id == "LAYR") {
      LwoLayer l;
      c.skip(4);
      l.pivot = {c.f32be(), c.f32be(), c.f32be()};
      l.name = lwo_string(c);
      layers.push_back(std::move(l));
    } else if (id == "PNTS") {
      CVT_TRY(check_records(ctx, size / 12, 12, size, "points"));
      while (c.remaining() >= 12) {
        const float x = c.f32be(), y = c.f32be(), z = c.f32be();
        layer().points.push_back({x, y, z});
      }
    } else if (id == "POLS") {
      const std::string_view type = c.chars(4);
      if (type != "FACE" && type != "PTCH") continue;  // curves, bones, ...
      while (c.remaining() >= 2) {
        const std::uint16_t count = c.u16be() & 0x03FF;
        std::vector<std::uint32_t> poly;
        for (std::uint16_t k = 0; k < count; ++k) poly.push_back(lwo_vx(c));
        if (!c.ok()) return fail(Status::Decode, "a LightWave polygon is truncated");
        layer().polygons.push_back(std::move(poly));
        CVT_TRY(check_elements(ctx, layer().polygons.size(), "polygons"));
      }
      layer().polygon_tag.assign(layer().polygons.size(), 0xFFFFFFFF);
    } else if (id == "PTAG") {
      if (c.chars(4) != "SURF") continue;
      while (c.remaining() >= 4) {
        const std::uint32_t poly = lwo_vx(c);
        const std::uint16_t tag = c.u16be();
        if (c.ok() && poly < layer().polygon_tag.size()) layer().polygon_tag[poly] = tag;
      }
    } else if (id == "VMAP" || id == "VMAD") {
      const std::string_view type = c.chars(4);
      const std::uint16_t dimension = c.u16be();
      (void)lwo_string(c);
      if (dimension > 4) continue;
      const bool per_poly = id == "VMAD";
      while (c.remaining() >= 2u + dimension * 4u) {
        const std::uint32_t point = lwo_vx(c);
        const std::uint32_t poly = per_poly ? lwo_vx(c) : 0;
        float v[4] = {0, 0, 0, 1};
        for (std::uint16_t k = 0; k < dimension; ++k) v[k] = c.f32be();
        if (!c.ok()) break;
        if (type == "TXUV" && dimension >= 2) {
          if (per_poly) layer().uv_poly[{poly, point}] = {v[0], v[1]};
          else layer().uv[point] = {v[0], v[1]};
        } else if ((type == "RGB " || type == "RGBA") && dimension >= 3 && !per_poly) {
          layer().color[point] = {v[0], v[1], v[2], dimension == 4 ? v[3] : 1.0f};
        }
      }
    } else if (id == "SURF") {
      ir::Material m;
      m.name = lwo_string(c);
      (void)lwo_string(c);  // source surface
      float diffuse = 1.0f;
      while (c.remaining() >= 6) {
        const std::string_view sub = c.chars(4);
        const std::uint16_t sub_size = c.u16be();
        util::ByteReader s(c.bytes(sub_size));
        if (sub_size % 2 != 0) c.skip(1);
        if (sub == "COLR" && sub_size >= 12) m.base_color = {s.f32be(), s.f32be(), s.f32be(), 1.0f};
        else if (sub == "DIFF" && sub_size >= 4) diffuse = s.f32be();
        else if (sub == "TRAN" && sub_size >= 4) m.base_color.a = std::clamp(1.0f - s.f32be(), 0.0f, 1.0f);
        else if (sub == "GLOS" && sub_size >= 4) m.roughness = std::clamp(1.0f - s.f32be(), 0.0f, 1.0f);
        else if (sub == "LUMI" && sub_size >= 4) {
          const float l = s.f32be();
          m.emissive = {l, l, l};
        }
      }
      m.base_color = {m.base_color.r * diffuse, m.base_color.g * diffuse, m.base_color.b * diffuse, m.base_color.a};
      surfaces[m.name] = m;
    }
    if (!r.ok()) return fail(Status::Decode, "the LightWave file is truncated");
  }
  ir::Mesh mesh;
  for (const auto& [name, m] : surfaces) mesh.materials.push_back(m);
  for (const LwoLayer& l : layers) {
    ir::Mesh part;
    std::unordered_map<CornerKey, std::uint32_t, CornerKeyHash> made;
    std::vector<std::string> face_material;
    for (std::size_t p = 0; p < l.polygons.size(); ++p) {
      const auto& poly = l.polygons[p];
      if (poly.size() < 3) continue;
      for (const std::uint32_t point : poly) {
        if (point >= l.points.size()) return fail(Status::Decode, "a LightWave polygon refers to missing point {}", point);
        const auto per_poly = l.uv_poly.find({static_cast<std::uint32_t>(p), point});
        const std::uint32_t variant = per_poly != l.uv_poly.end() ? static_cast<std::uint32_t>(p) + 1 : 0;
        const auto [it, inserted] = made.emplace(CornerKey{point, variant, 0}, static_cast<std::uint32_t>(part.positions.size()));
        if (inserted) {
          const ir::Vec3& q = l.points[point];
          part.positions.push_back({q.x, q.y, -q.z});  // left-handed -> right-handed
          ir::Vec2 uv{};
          if (per_poly != l.uv_poly.end()) uv = per_poly->second;
          else if (const auto u = l.uv.find(point); u != l.uv.end()) uv = u->second;
          part.texcoords.push_back(uv);
          const auto col = l.color.find(point);
          part.colors.push_back(col != l.color.end() ? col->second : ir::Color{});
        }
        part.indices.push_back(it->second);
      }
      part.face_sizes.push_back(static_cast<std::uint32_t>(poly.size()));
      const std::uint32_t tag = p < l.polygon_tag.size() ? l.polygon_tag[p] : 0xFFFFFFFF;
      face_material.push_back(tag < tags.size() && surfaces.contains(tags[tag]) ? tags[tag] : std::string());
    }
    if (part.positions.empty()) continue;
    if (l.uv.empty() && l.uv_poly.empty()) part.texcoords.clear();
    if (l.color.empty()) part.colors.clear();
    for (std::size_t f = 0; f < face_material.size(); ++f) {
      if (part.groups.empty() || part.groups.back().material != face_material[f]) part.groups.push_back({l.name, face_material[f], f, 0});
      part.groups.back().face_count++;
    }
    const std::size_t first = mesh.face_count();
    const auto materials = std::move(mesh.materials);
    append(mesh, part);
    mesh.materials = materials;
    mesh.objects.push_back({l.name.empty() ? std::format("layer_{}", mesh.objects.size()) : l.name, first, mesh.face_count() - first,
                            translation(l.pivot.x, l.pivot.y, -l.pivot.z)});
  }
  if (mesh.objects.size() == 1) mesh.objects.clear();
  mesh.source_up_axis = "Y";
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- Quake MD2 / MD3

// The 162 precomputed MD2 vertex normals are not needed: normals are
// recomputed from the geometry, which matches them closely.

Result<AnyIR> read_md2(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("IDP2") || r.i32le() != 8) return fail(Status::Decode, "not a Quake II MD2 model");
  std::int32_t h[15];
  for (std::int32_t& v : h) v = r.i32le();
  if (!r.ok()) return fail(Status::Decode, "the MD2 header is truncated");
  const std::int32_t skin_w = h[0], skin_h = h[1], frame_size = h[2], skins = h[3], xyz = h[4], st = h[5], tris = h[6],
                     frames = h[8], ofs_skins = h[9], ofs_st = h[10], ofs_tris = h[11], ofs_frames = h[12];
  const auto valid = [&](std::int64_t offset, std::int64_t count, std::int64_t each) {
    return offset >= 0 && count >= 0 && each > 0 && count <= (static_cast<std::int64_t>(ctx.input.size()) - offset) / each;
  };
  if (frames < 1 || xyz < 1 || !valid(ofs_st, st, 4) || !valid(ofs_tris, tris, 12) || !valid(ofs_frames, 1, frame_size) ||
      static_cast<std::int64_t>(frame_size) < 40 + 4 * static_cast<std::int64_t>(xyz) || !valid(ofs_skins, skins, 64)) {
    return fail(Status::Decode, "the MD2 header is inconsistent");
  }
  CVT_TRY(check_elements(ctx, static_cast<std::uint64_t>(tris) * 3, "corners"));
  if (frames > 1) ctx.warn(std::format("The MD2 model has {} animation frames; only the first was converted.", frames));
  util::ByteReader fr(ctx.input.subspan(static_cast<std::size_t>(ofs_frames), static_cast<std::size_t>(frame_size)));
  const float sx = fr.f32le(), sy = fr.f32le(), sz = fr.f32le(), tx = fr.f32le(), ty = fr.f32le(), tz = fr.f32le();
  fr.skip(16);
  std::vector<ir::Vec3> points;
  for (std::int32_t i = 0; i < xyz; ++i) {
    const std::uint8_t x = fr.u8(), y = fr.u8(), z = fr.u8();
    fr.skip(1);
    points.push_back({x * sx + tx, y * sy + ty, z * sz + tz});
  }
  std::vector<ir::Vec2> uvs;
  util::ByteReader sr(ctx.input.subspan(static_cast<std::size_t>(ofs_st), static_cast<std::size_t>(st) * 4));
  for (std::int32_t i = 0; i < st; ++i) {
    const float s = sr.i16le(), t = sr.i16le();
    uvs.push_back({skin_w > 0 ? s / static_cast<float>(skin_w) : 0.0f, skin_h > 0 ? 1.0f - t / static_cast<float>(skin_h) : 0.0f});
  }
  ir::Mesh mesh;
  std::unordered_map<CornerKey, std::uint32_t, CornerKeyHash> made;
  util::ByteReader tr(ctx.input.subspan(static_cast<std::size_t>(ofs_tris), static_cast<std::size_t>(tris) * 12));
  for (std::int32_t t = 0; t < tris; ++t) {
    std::uint16_t v[3], s[3];
    for (std::uint16_t& x : v) x = tr.u16le();
    for (std::uint16_t& x : s) x = tr.u16le();
    // Quake winds front faces clockwise: reverse to counter-clockwise.
    for (const int k : {0, 2, 1}) {
      if (v[k] >= points.size()) return fail(Status::Decode, "an MD2 triangle refers to missing vertex {}", v[k]);
      const std::uint32_t uv = s[k] < uvs.size() ? s[k] : 0xFFFF;
      const auto [it, inserted] = made.emplace(CornerKey{v[k], uv, 0}, static_cast<std::uint32_t>(mesh.positions.size()));
      if (inserted) {
        mesh.positions.push_back(points[v[k]]);
        mesh.texcoords.push_back(uv < uvs.size() ? uvs[uv] : ir::Vec2{});
      }
      mesh.indices.push_back(it->second);
    }
  }
  util::ByteReader kr(ctx.input.subspan(static_cast<std::size_t>(ofs_skins), static_cast<std::size_t>(skins) * 64));
  for (std::int32_t i = 0; i < skins; ++i) {
    ir::Texture texture;
    texture.uri = fixed_string(kr, 64);
    texture.name = std::string(base_name(texture.uri));
    mesh.textures.push_back(std::move(texture));
  }
  if (!mesh.textures.empty()) {
    ir::Material skin;
    skin.name = "skin";
    skin.base_color_texture = 0;
    mesh.materials.push_back(skin);
    mesh.groups.push_back({"model", "skin", 0, mesh.face_count()});
    ctx.warn("MD2 skins are separate image files and were not included.");
  }
  if (mesh.positions.empty()) return fail(Status::Decode, "the MD2 model has no triangles");
  mesh.source_up_axis = "Z";
  convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  generate_normals(mesh, 60.0f);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

Result<AnyIR> read_md3(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("IDP3") || r.i32le() != 15) return fail(Status::Decode, "not a Quake III MD3 model");
  r.skip(64 + 4);
  const std::int32_t frames = r.i32le(), tags = r.i32le(), surfaces = r.i32le();
  r.skip(4 + 4 + 4);
  const std::int32_t ofs_surfaces = r.i32le();
  (void)tags;
  if (!r.ok() || surfaces < 0 || surfaces > 1024 || ofs_surfaces < 0 || static_cast<std::size_t>(ofs_surfaces) > ctx.input.size()) {
    return fail(Status::Decode, "the MD3 header is inconsistent");
  }
  if (frames > 1) ctx.warn(std::format("The MD3 model has {} animation frames; only the first was converted.", frames));
  ir::Mesh mesh;
  std::size_t at = static_cast<std::size_t>(ofs_surfaces);
  for (std::int32_t s = 0; s < surfaces; ++s) {
    util::ByteReader h(ctx.input.subspan(std::min(at, ctx.input.size())));
    if (!h.consume("IDP3")) return fail(Status::Decode, "an MD3 surface header is missing");
    const std::string name = fixed_string(h, 64);
    h.skip(4);
    const std::int32_t n_frames = h.i32le(), n_shaders = h.i32le(), n_verts = h.i32le(), n_tris = h.i32le();
    const std::int32_t ofs_tris = h.i32le(), ofs_shaders = h.i32le(), ofs_st = h.i32le(), ofs_xyz = h.i32le(), ofs_end = h.i32le();
    const std::size_t surface_size = ctx.input.size() - at;
    const auto valid = [&](std::int64_t offset, std::int64_t count, std::int64_t each) {
      return offset >= 0 && count >= 0 && count <= (static_cast<std::int64_t>(surface_size) - offset) / each;
    };
    if (!h.ok() || n_frames < 1 || !valid(ofs_tris, n_tris, 12) || !valid(ofs_st, n_verts, 8) || !valid(ofs_xyz, n_verts, 8) ||
        !valid(ofs_shaders, n_shaders, 68) || ofs_end <= 0 || static_cast<std::size_t>(ofs_end) > surface_size) {
      return fail(Status::Decode, "an MD3 surface is inconsistent");
    }
    CVT_TRY(check_elements(ctx, mesh.positions.size() + static_cast<std::uint64_t>(n_verts), "vertices"));
    ir::Mesh part;
    const ByteView surface = ctx.input.subspan(at);
    util::ByteReader xr(surface.subspan(static_cast<std::size_t>(ofs_xyz)));
    util::ByteReader sr(surface.subspan(static_cast<std::size_t>(ofs_st)));
    for (std::int32_t v = 0; v < n_verts; ++v) {
      const float x = xr.i16le() / 64.0f, y = xr.i16le() / 64.0f, z = xr.i16le() / 64.0f;
      const std::uint16_t packed = xr.u16le();
      const double lat = ((packed >> 8) & 0xFF) * 2 * std::numbers::pi / 255.0;
      const double lng = (packed & 0xFF) * 2 * std::numbers::pi / 255.0;
      part.positions.push_back({x, y, z});
      part.normals.push_back({static_cast<float>(std::cos(lat) * std::sin(lng)), static_cast<float>(std::sin(lat) * std::sin(lng)),
                              static_cast<float>(std::cos(lng))});
      const float u = sr.f32le(), t = sr.f32le();
      part.texcoords.push_back({u, 1.0f - t});
    }
    util::ByteReader tr(surface.subspan(static_cast<std::size_t>(ofs_tris)));
    for (std::int32_t t = 0; t < n_tris; ++t) {
      const std::int32_t a = tr.i32le(), b = tr.i32le(), c = tr.i32le();
      if (a < 0 || b < 0 || c < 0 || a >= n_verts || b >= n_verts || c >= n_verts) return fail(Status::Decode, "an MD3 triangle is out of range");
      // Clockwise in Quake: reverse.
      part.indices.insert(part.indices.end(), {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(b)});
    }
    std::string material;
    if (n_shaders > 0) {
      util::ByteReader shr(surface.subspan(static_cast<std::size_t>(ofs_shaders)));
      ir::Texture texture;
      texture.uri = fixed_string(shr, 64);
      texture.name = std::string(base_name(texture.uri));
      ir::Material m;
      m.name = name.empty() ? std::format("surface_{}", s) : name;
      m.base_color_texture = static_cast<int>(mesh.textures.size());
      mesh.textures.push_back(std::move(texture));
      mesh.materials.push_back(m);
      material = m.name;
    }
    part.groups.push_back({name, material, 0, part.face_count()});
    const std::size_t first = mesh.face_count();
    const auto materials = std::move(mesh.materials);
    const auto textures = std::move(mesh.textures);
    append(mesh, part);
    mesh.materials = materials;
    mesh.textures = textures;
    mesh.objects.push_back({name, first, mesh.face_count() - first, {}});
    at += static_cast<std::size_t>(ofs_end);
  }
  if (mesh.positions.empty()) return fail(Status::Decode, "the MD3 model has no geometry");
  if (!mesh.textures.empty()) ctx.warn("MD3 shaders refer to separate image files, which were not included.");
  if (mesh.objects.size() == 1) mesh.objects.clear();
  mesh.source_up_axis = "Z";
  convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- MilkShape 3D

Result<AnyIR> read_ms3d(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("MS3D000000")) return fail(Status::Decode, "not a MilkShape 3D model");
  const std::int32_t version = r.i32le();
  if (version < 3 || version > 4) return fail(Status::Decode, "MilkShape 3D version {} is not supported", version);
  const std::uint16_t n_vertices = r.u16le();
  CVT_TRY(check_records(ctx, n_vertices, 15, r.remaining(), "vertices"));
  std::vector<ir::Vec3> points;
  for (std::uint16_t i = 0; i < n_vertices; ++i) {
    r.skip(1);
    const float x = r.f32le(), y = r.f32le(), z = r.f32le();
    points.push_back({x, y, z});
    r.skip(2);
  }
  const std::uint16_t n_tris = r.u16le();
  CVT_TRY(check_records(ctx, n_tris, 70, r.remaining(), "triangles"));
  ir::Mesh mesh;
  struct Corner {
    std::uint16_t vertex;
    ir::Vec3 normal;
    ir::Vec2 uv;
  };
  std::vector<std::array<Corner, 3>> triangles(n_tris);
  for (auto& tri : triangles) {
    r.skip(2);
    for (Corner& c : tri) c.vertex = r.u16le();
    for (Corner& c : tri) c.normal = {r.f32le(), r.f32le(), r.f32le()};
    for (Corner& c : tri) c.uv.x = r.f32le();
    for (Corner& c : tri) c.uv.y = 1.0f - r.f32le();
    r.skip(2);  // smoothing group, group index
  }
  const std::uint16_t n_groups = r.u16le();
  struct Group {
    std::string name;
    std::vector<std::uint16_t> triangles;
    std::int8_t material = -1;
  };
  std::vector<Group> groups;
  for (std::uint16_t g = 0; g < n_groups && r.ok(); ++g) {
    Group group;
    r.skip(1);
    group.name = fixed_string(r, 32);
    const std::uint16_t count = r.u16le();
    if (count > r.remaining() / 2) return fail(Status::Decode, "a MilkShape group is truncated");
    for (std::uint16_t k = 0; k < count; ++k) group.triangles.push_back(r.u16le());
    group.material = r.i8();
    groups.push_back(std::move(group));
  }
  const std::uint16_t n_materials = r.ok() ? r.u16le() : 0;
  std::vector<std::string> material_names;
  for (std::uint16_t m = 0; m < n_materials && r.ok(); ++m) {
    ir::Material material;
    material.name = fixed_string(r, 32);
    r.skip(16);  // ambient
    material.base_color = {r.f32le(), r.f32le(), r.f32le(), r.f32le()};
    r.skip(16);  // specular
    const float er = r.f32le(), eg = r.f32le(), eb = r.f32le();
    r.skip(4);
    material.emissive = {er, eg, eb};
    const float shininess = r.f32le();
    material.roughness = std::clamp(1.0f - shininess / 128.0f, 0.0f, 1.0f);
    material.base_color.a = std::clamp(r.f32le(), 0.0f, 1.0f);
    r.skip(1);
    const std::string texture = fixed_string(r, 128);
    r.skip(128);
    if (!texture.empty()) {
      material.base_color_texture = static_cast<int>(mesh.textures.size());
      mesh.textures.push_back({std::string(base_name(texture)), texture, "", {}});
    }
    if (material.name.empty()) material.name = std::format("material_{}", m);
    material_names.push_back(material.name);
    mesh.materials.push_back(material);
  }
  if (!r.ok()) return fail(Status::Decode, "the MilkShape 3D file is truncated");
  // Triangles in group order (each triangle belongs to one group).
  std::vector<bool> used(n_tris, false);
  std::map<std::tuple<std::uint16_t, float, float, float, float, float>, std::uint32_t> corners;
  const auto emit = [&](std::size_t t) -> Result<void> {
    for (const Corner& c : triangles[t]) {
      if (c.vertex >= points.size()) return fail(Status::Decode, "a MilkShape triangle refers to missing vertex {}", c.vertex);
      const auto key = std::make_tuple(c.vertex, c.normal.x, c.normal.y, c.normal.z, c.uv.x, c.uv.y);
      const auto [it, inserted] = corners.emplace(key, static_cast<std::uint32_t>(mesh.positions.size()));
      if (inserted) {
        mesh.positions.push_back(points[c.vertex]);
        mesh.normals.push_back(c.normal);
        mesh.texcoords.push_back(c.uv);
      }
      mesh.indices.push_back(it->second);
    }
    return {};
  };
  for (const Group& g : groups) {
    const std::size_t first = mesh.face_count();
    for (const std::uint16_t t : g.triangles) {
      if (t >= n_tris || used[t]) continue;
      used[t] = true;
      CVT_TRY(emit(t));
    }
    const std::string material = g.material >= 0 && static_cast<std::size_t>(g.material) < material_names.size() ? material_names[g.material] : "";
    if (mesh.face_count() > first) mesh.groups.push_back({g.name, material, first, mesh.face_count() - first});
  }
  for (std::size_t t = 0; t < n_tris; ++t) {
    if (!used[t]) CVT_TRY(emit(t));
  }
  if (!mesh.textures.empty()) ctx.warn("MilkShape textures are separate image files and were not included.");
  if (mesh.positions.empty()) return fail(Status::Decode, "the MilkShape 3D model has no triangles");
  mesh.source_up_axis = "Y";
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- DirectX .x (text)

struct XNode {
  std::string type;
  std::string name;
  std::vector<double> numbers;
  std::vector<std::string> strings;
  std::vector<std::string> references;
  std::vector<XNode> children;
};

class XParser {
 public:
  XParser(std::string_view text, const ReadContext& ctx) : text_(text), ctx_(ctx) {}

  Result<std::vector<XNode>> parse() {
    std::vector<XNode> out;
    for (;;) {
      skip();
      if (pos_ >= text_.size()) break;
      XNode node;
      CVT_TRY(object(node, 0));
      if (node.type != "template") out.push_back(std::move(node));
    }
    return out;
  }

 private:
  void skip() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '#' || (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/')) {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
      } else if (util::is_ascii_space(c) || c == ',' || c == ';') {
        ++pos_;
      } else {
        break;
      }
    }
  }
  std::string_view word() {
    skip();
    const std::size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (util::is_ascii_space(c) || c == ',' || c == ';' || c == '{' || c == '}' || c == '"') break;
      ++pos_;
    }
    return text_.substr(start, pos_ - start);
  }

  Result<void> object(XNode& node, int depth) {
    if (depth > 64) return fail(Status::Decode, "DirectX objects are nested too deeply");
    if (++objects_ > 1'000'000) return fail(Status::Limit, "the DirectX file has too many objects");
    node.type = std::string(word());
    skip();
    if (pos_ < text_.size() && text_[pos_] != '{') node.name = std::string(word());
    skip();
    if (pos_ >= text_.size() || text_[pos_] != '{') return fail(Status::Decode, "malformed DirectX object '{}'", node.type);
    ++pos_;
    if (node.type == "template") {
      // Skip the template body.
      int level = 1;
      while (pos_ < text_.size() && level > 0) {
        if (text_[pos_] == '{') ++level;
        else if (text_[pos_] == '}') --level;
        ++pos_;
      }
      if (level != 0) return fail(Status::Decode, "unterminated DirectX template");
      return {};
    }
    for (;;) {
      skip();
      if (pos_ >= text_.size()) return fail(Status::Decode, "the DirectX file is truncated");
      const char c = text_[pos_];
      if (c == '}') {
        ++pos_;
        return {};
      }
      if (c == '<') {  // GUID
        while (pos_ < text_.size() && text_[pos_] != '>') ++pos_;
        ++pos_;
        continue;
      }
      if (c == '"') {
        const std::size_t start = ++pos_;
        while (pos_ < text_.size() && text_[pos_] != '"') ++pos_;
        node.strings.emplace_back(text_.substr(start, pos_ - start));
        if (pos_ < text_.size()) ++pos_;
        continue;
      }
      if (c == '{') {  // reference: { Name }
        ++pos_;
        const std::string_view name = word();
        skip();
        if (pos_ < text_.size() && text_[pos_] == '}') ++pos_;
        node.references.emplace_back(name);
        continue;
      }
      if (util::is_ascii_digit(c) || c == '-' || c == '+' || c == '.') {
        double v = 0;
        const std::string_view token = word();
        if (!parse_double(token, v)) return fail(Status::Decode, "malformed DirectX number '{}'", token);
        node.numbers.push_back(v);
        if ((node.numbers.size() & 0xFFFF) == 0) CVT_TRY(check_elements(ctx_, node.numbers.size(), "DirectX values"));
        continue;
      }
      XNode child;
      CVT_TRY(object(child, depth + 1));
      node.children.push_back(std::move(child));
    }
  }

  std::string_view text_;
  const ReadContext& ctx_;
  std::size_t pos_ = 0;
  std::size_t objects_ = 0;
};

class XScene {
 public:
  XScene(const std::vector<XNode>& top, ReadContext& ctx) : ctx_(ctx) {
    for (const XNode& n : top) index(n);
  }

  Result<ir::Mesh> build(const std::vector<XNode>& top) {
    for (const XNode& n : top) CVT_TRY(visit(n, {}, 0));
    return std::move(mesh_);
  }

 private:
  void index(const XNode& n) {
    if (!n.name.empty() && n.type == "Material") materials_[n.name] = &n;
    for (const XNode& c : n.children) index(c);
  }

  std::string material(const XNode& node) {
    const auto key = &node;
    if (const auto it = made_.find(key); it != made_.end()) return it->second;
    ir::Material m;
    m.name = node.name.empty() ? std::format("material_{}", mesh_.materials.size()) : node.name;
    for (int k = 2; mesh_.find_material(m.name) != nullptr; ++k) m.name = std::format("{}_{}", node.name, k);
    const auto& v = node.numbers;
    if (v.size() >= 4) m.base_color = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])};
    if (v.size() >= 5) m.roughness = static_cast<float>(std::clamp(std::sqrt(2.0 / (v[4] + 2.0)), 0.0, 1.0));
    if (v.size() >= 11) m.emissive = {static_cast<float>(v[8]), static_cast<float>(v[9]), static_cast<float>(v[10])};
    for (const XNode& c : node.children) {
      if ((c.type == "TextureFilename" || c.type == "TextureFileName") && !c.strings.empty()) {
        m.base_color_texture = static_cast<int>(mesh_.textures.size());
        mesh_.textures.push_back({std::string(base_name(c.strings[0])), c.strings[0], "", {}});
        ctx_.warn(std::format("Texture '{}' is a separate file and was not included.", c.strings[0]));
      }
    }
    made_[key] = m.name;
    mesh_.materials.push_back(std::move(m));
    return made_[key];
  }

  Result<void> visit(const XNode& node, const ir::Mat4& parent, int depth) {
    if (depth > 64) return fail(Status::Decode, "DirectX frames are nested too deeply");
    if (node.type == "Frame") {
      ir::Mat4 local;
      for (const XNode& c : node.children) {
        if (c.type == "FrameTransformMatrix" && c.numbers.size() >= 16) {
          for (int i = 0; i < 16; ++i) local.m[i] = static_cast<float>(c.numbers[static_cast<std::size_t>(i)]);
        }
      }
      const ir::Mat4 world = parent * local;
      for (const XNode& c : node.children) {
        if (c.type == "Frame") CVT_TRY(visit(c, world, depth + 1));
        else if (c.type == "Mesh") CVT_TRY(mesh(c, world, node.name));
      }
    } else if (node.type == "Mesh") {
      CVT_TRY(mesh(node, parent, node.name));
    }
    return {};
  }

  Result<void> mesh(const XNode& node, const ir::Mat4& world, const std::string& frame_name) {
    const auto& v = node.numbers;
    std::size_t at = 0;
    const auto next = [&](double& out) {
      if (at >= v.size()) return false;
      out = v[at++];
      return true;
    };
    double count = 0;
    if (!next(count) || !(count >= 0) || count * 3 > static_cast<double>(v.size())) return fail(Status::Decode, "malformed DirectX mesh");
    std::vector<ir::Vec3> points;
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
      points.push_back({static_cast<float>(v[at]), static_cast<float>(v[at + 1]), static_cast<float>(v[at + 2])});
      at += 3;
    }
    double faces = 0;
    if (!next(faces) || !(faces >= 0) || faces > static_cast<double>(v.size())) return fail(Status::Decode, "malformed DirectX mesh faces");
    std::vector<std::vector<std::uint32_t>> polys;
    for (std::size_t f = 0; f < static_cast<std::size_t>(faces); ++f) {
      double n = 0;
      if (!next(n) || !(n >= 0) || n > 1e6) return fail(Status::Decode, "malformed DirectX face");
      std::vector<std::uint32_t> poly;
      for (std::size_t k = 0; k < static_cast<std::size_t>(n); ++k) {
        double index = 0;
        if (!next(index) || !(index >= 0) || index >= count) return fail(Status::Decode, "a DirectX face refers to a missing vertex");
        poly.push_back(static_cast<std::uint32_t>(index));
      }
      polys.push_back(std::move(poly));
    }
    // Children: normals (own face list), uvs, colours, materials.
    std::vector<ir::Vec3> normals;
    std::vector<std::vector<std::uint32_t>> normal_faces;
    std::vector<ir::Vec2> uvs;
    std::map<std::uint32_t, ir::Color> colors;
    std::vector<std::string> face_material(polys.size());
    for (const XNode& c : node.children) {
      const auto& n = c.numbers;
      if (c.type == "MeshNormals" && !n.empty()) {
        std::size_t k = 0;
        const auto count_n = static_cast<std::size_t>(std::min<std::uint64_t>(file_index(n[k++]), n.size()));
        if (count_n * 3 + 1 > n.size()) return fail(Status::Decode, "malformed DirectX normals");
        for (std::size_t i = 0; i < count_n; ++i, k += 3) normals.push_back({static_cast<float>(n[k]), static_cast<float>(n[k + 1]), static_cast<float>(n[k + 2])});
        if (k < n.size()) {
          const auto count_f = static_cast<std::size_t>(std::min<std::uint64_t>(file_index(n[k++]), n.size()));
          for (std::size_t f = 0; f < count_f && k < n.size(); ++f) {
            const auto m = static_cast<std::size_t>(std::min<std::uint64_t>(file_index(n[k++]), n.size()));
            std::vector<std::uint32_t> face;
            for (std::size_t j = 0; j < m && k < n.size(); ++j) face.push_back(static_cast<std::uint32_t>(std::min<std::uint64_t>(file_index(n[k++]), 0xFFFFFFFFu)));
            normal_faces.push_back(std::move(face));
          }
        }
      } else if (c.type == "MeshTextureCoords" && !n.empty()) {
        const auto count_t = static_cast<std::size_t>(std::min<std::uint64_t>(file_index(n[0]), n.size()));
        for (std::size_t i = 0; i < count_t && 2 + i * 2 < n.size(); ++i) {
          uvs.push_back({static_cast<float>(n[1 + i * 2]), 1.0f - static_cast<float>(n[2 + i * 2])});
        }
      } else if (c.type == "MeshVertexColors" && !n.empty()) {
        const auto count_c = static_cast<std::size_t>(std::min<std::uint64_t>(file_index(n[0]), n.size()));
        for (std::size_t i = 0; i < count_c && 1 + i * 5 + 4 < n.size(); ++i) {
          const std::size_t k = 1 + i * 5;
          colors[static_cast<std::uint32_t>(std::min<std::uint64_t>(file_index(n[k]), 0xFFFFFFFFu))] = {static_cast<float>(n[k + 1]), static_cast<float>(n[k + 2]),
                                                      static_cast<float>(n[k + 3]), static_cast<float>(n[k + 4])};
        }
      } else if (c.type == "MeshMaterialList" && n.size() >= 2) {
        std::vector<std::string> names;
        for (const XNode& m : c.children) {
          if (m.type == "Material") names.push_back(material(m));
        }
        for (const std::string& ref : c.references) {
          const auto it = materials_.find(ref);
          if (it != materials_.end()) names.push_back(material(*it->second));
        }
        const auto count_i = static_cast<std::size_t>(std::min<std::uint64_t>(file_index(n[1]), n.size()));
        for (std::size_t f = 0; f < face_material.size(); ++f) {
          // A single index applies to every face.
          const std::size_t k = 2 + std::min(f, count_i > 0 ? count_i - 1 : 0);
          if (k < n.size()) {
            const auto m = static_cast<std::size_t>(std::min<std::uint64_t>(file_index(n[k]), names.size()));
            if (m < names.size()) face_material[f] = names[m];
          }
        }
      }
    }
    ir::Mesh part;
    std::unordered_map<CornerKey, std::uint32_t, CornerKeyHash> made;
    const bool use_normals = normal_faces.size() == polys.size();
    for (std::size_t f = 0; f < polys.size(); ++f) {
      const auto& poly = polys[f];
      if (poly.size() < 3) continue;
      // Left-handed: mirror z and reverse the corner order.
      for (std::size_t k = poly.size(); k > 0; --k) {
        const std::size_t corner = (k == poly.size()) ? 0 : k;
        const std::uint32_t point = poly[corner];
        const std::uint32_t ni = use_normals && corner < normal_faces[f].size() ? normal_faces[f][corner] : 0xFFFFFFFF;
        const auto [it, inserted] = made.emplace(CornerKey{point, ni, 0}, static_cast<std::uint32_t>(part.positions.size()));
        if (inserted) {
          const ir::Vec3& p = points[point];
          part.positions.push_back({p.x, p.y, -p.z});
          if (use_normals) {
            const ir::Vec3 n = ni < normals.size() ? normals[ni] : ir::Vec3{};
            part.normals.push_back({n.x, n.y, -n.z});
          }
          if (uvs.size() == points.size()) part.texcoords.push_back(uvs[point]);
          if (!colors.empty()) {
            const auto c = colors.find(point);
            part.colors.push_back(c != colors.end() ? c->second : ir::Color{});
          }
        }
        part.indices.push_back(it->second);
      }
      part.face_sizes.push_back(static_cast<std::uint32_t>(poly.size()));
      if (part.groups.empty() || part.groups.back().material != face_material[f]) {
        part.groups.push_back({frame_name, face_material[f], part.face_sizes.size() - 1, 0});
      }
      part.groups.back().face_count++;
    }
    // The frame matrix of a left-handed file, mirrored like the points.
    const ir::Mat4 mirror = scaling(1, 1, -1);
    transform(part, mirror * world * mirror);
    const std::size_t first = mesh_.face_count();
    const auto materials = std::move(mesh_.materials);
    const auto textures = std::move(mesh_.textures);
    append(mesh_, part);
    mesh_.materials = materials;
    mesh_.textures = textures;
    mesh_.objects.push_back({frame_name, first, mesh_.face_count() - first, mirror * world * mirror});
    CVT_TRY(check_elements(ctx_, mesh_.positions.size(), "vertices"));
    return {};
  }

  ReadContext& ctx_;
  ir::Mesh mesh_;
  std::map<std::string, const XNode*> materials_;
  std::map<const XNode*, std::string> made_;
};

Result<AnyIR> read_x(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  if (!text.starts_with("xof ") || text.size() < 16) return fail(Status::Decode, "not a DirectX file");
  const std::string_view format = text.substr(8, 4);
  if (format != "txt ") return fail(Status::Decode, "binary and compressed DirectX files are not supported (only text .x files)");
  XParser parser(text.substr(16), ctx);
  CVT_TRY_ASSIGN(const std::vector<XNode> top, parser.parse());
  XScene scene(top, ctx);
  CVT_TRY_ASSIGN(ir::Mesh mesh, scene.build(top));
  if (mesh.positions.empty()) return fail(Status::Decode, "the DirectX file contains no meshes");
  if (mesh.objects.size() == 1 && mesh.objects[0].transform.is_identity()) mesh.objects.clear();
  mesh.source_up_axis = "Y";
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

// --------------------------------------------------------------------------- MagicaVoxel

// The default MagicaVoxel palette (ABGR words): a 6x6x6 colour cube without
// black, then ramps of red, green, blue and grey.
std::array<std::uint32_t, 256> default_vox_palette() {
  std::array<std::uint32_t, 256> p{};
  std::size_t i = 1;
  static constexpr std::uint32_t steps[] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00};
  for (const std::uint32_t b : steps) {
    for (const std::uint32_t g : steps) {
      for (const std::uint32_t r : steps) {
        if (b == 0 && g == 0 && r == 0) continue;
        p[i++] = 0xFF000000u | (b << 16) | (g << 8) | r;
      }
    }
  }
  static constexpr std::uint32_t ramp[] = {0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
  for (const std::uint32_t v : ramp) p[i++] = 0xFF000000u | v;
  for (const std::uint32_t v : ramp) p[i++] = 0xFF000000u | (v << 8);
  for (const std::uint32_t v : ramp) p[i++] = 0xFF000000u | (v << 16);
  for (const std::uint32_t v : ramp) p[i++] = 0xFF000000u | (v << 16) | (v << 8) | v;
  return p;
}

struct VoxModel {
  std::int32_t sx = 0, sy = 0, sz = 0;
  std::vector<std::uint8_t> cells;  // colour index per cell, 0 = empty
};

// Greedy meshing of exposed faces: per axis and direction, merge maximal
// rectangles of equal colour in every slice.
void greedy_mesh(const VoxModel& model, const std::array<ir::Color, 256>& colors, float offset_x, ir::Mesh& out) {
  const std::int32_t dims[3] = {model.sx, model.sy, model.sz};
  const auto cell = [&](std::int32_t x, std::int32_t y, std::int32_t z) -> std::uint8_t {
    if (x < 0 || y < 0 || z < 0 || x >= model.sx || y >= model.sy || z >= model.sz) return 0;
    return model.cells[(static_cast<std::size_t>(z) * model.sy + static_cast<std::size_t>(y)) * model.sx + static_cast<std::size_t>(x)];
  };
  for (int axis = 0; axis < 3; ++axis) {
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    for (int side = 0; side < 2; ++side) {
      std::vector<std::uint8_t> mask(static_cast<std::size_t>(dims[u]) * dims[v]);
      for (std::int32_t d = 0; d < dims[axis]; ++d) {
        // Faces of cells in slice d that look towards `side` and are exposed.
        for (std::int32_t j = 0; j < dims[v]; ++j) {
          for (std::int32_t i = 0; i < dims[u]; ++i) {
            std::int32_t p[3] = {0, 0, 0};
            p[axis] = d;
            p[u] = i;
            p[v] = j;
            const std::uint8_t here = cell(p[0], p[1], p[2]);
            p[axis] += side == 0 ? -1 : 1;
            const std::uint8_t neighbour = cell(p[0], p[1], p[2]);
            mask[static_cast<std::size_t>(j) * dims[u] + i] = here != 0 && neighbour == 0 ? here : 0;
          }
        }
        for (std::int32_t j = 0; j < dims[v]; ++j) {
          for (std::int32_t i = 0; i < dims[u];) {
            const std::uint8_t c = mask[static_cast<std::size_t>(j) * dims[u] + i];
            if (c == 0) {
              ++i;
              continue;
            }
            std::int32_t w = 1;
            while (i + w < dims[u] && mask[static_cast<std::size_t>(j) * dims[u] + i + w] == c) ++w;
            std::int32_t h = 1;
            for (; j + h < dims[v]; ++h) {
              bool row = true;
              for (std::int32_t k = 0; k < w && row; ++k) row = mask[static_cast<std::size_t>(j + h) * dims[u] + i + k] == c;
              if (!row) break;
            }
            for (std::int32_t y = 0; y < h; ++y) {
              for (std::int32_t x = 0; x < w; ++x) mask[static_cast<std::size_t>(j + y) * dims[u] + i + x] = 0;
            }
            // Quad corners.
            const float plane = static_cast<float>(d + (side == 0 ? 0 : 1));
            const auto corner = [&](std::int32_t du, std::int32_t dv) {
              float q[3];
              q[axis] = plane;
              q[u] = static_cast<float>(i + du);
              q[v] = static_cast<float>(j + dv);
              return ir::Vec3{q[0] + offset_x, q[1], q[2]};
            };
            const auto base = static_cast<std::uint32_t>(out.positions.size());
            out.positions.insert(out.positions.end(), {corner(0, 0), corner(w, 0), corner(w, h), corner(0, h)});
            out.colors.insert(out.colors.end(), 4, colors[c]);
            // (u, v, axis) is right-handed: counter-clockwise faces +axis.
            if (side == 1) out.indices.insert(out.indices.end(), {base, base + 1, base + 2, base + 3});
            else out.indices.insert(out.indices.end(), {base, base + 3, base + 2, base + 1});
            out.face_sizes.push_back(4);
            i += w;
          }
        }
      }
    }
  }
}

Result<AnyIR> read_vox(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("VOX ")) return fail(Status::Decode, "not a MagicaVoxel file");
  r.skip(4);
  if (!r.consume("MAIN")) return fail(Status::Decode, "the MagicaVoxel file has no MAIN chunk");
  const std::uint32_t main_content = r.u32le();
  const std::uint32_t main_children = r.u32le();
  r.skip(main_content);
  if (!r.ok() || main_children > r.remaining()) return fail(Status::Decode, "the MagicaVoxel file is truncated");
  std::vector<VoxModel> models;
  std::array<std::uint32_t, 256> palette = default_vox_palette();
  bool custom_palette = false;
  std::size_t scene_nodes = 0;
  while (r.remaining() >= 12) {
    const std::string_view id = r.chars(4);
    const std::uint32_t content = r.u32le();
    const std::uint32_t child_size = r.u32le();
    if (content > r.remaining() || child_size > r.remaining() - content) return fail(Status::Decode, "a MagicaVoxel chunk is truncated");
    util::ByteReader c(r.bytes(content));
    r.skip(child_size);
    if (id == "SIZE") {
      VoxModel m;
      m.sx = c.i32le();
      m.sy = c.i32le();
      m.sz = c.i32le();
      if (!c.ok() || m.sx <= 0 || m.sy <= 0 || m.sz <= 0 || m.sx > 256 || m.sy > 256 || m.sz > 256) {
        return fail(Status::Decode, "invalid MagicaVoxel model size");
      }
      CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(m.sx) * m.sy * m.sz * (models.size() + 1)));
      m.cells.assign(static_cast<std::size_t>(m.sx) * m.sy * m.sz, 0);
      models.push_back(std::move(m));
    } else if (id == "XYZI") {
      if (models.empty()) return fail(Status::Decode, "MagicaVoxel voxels before their size");
      VoxModel& m = models.back();
      const std::uint32_t n = c.u32le();
      if (n > c.remaining() / 4) return fail(Status::Decode, "the MagicaVoxel voxel list is truncated");
      for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint8_t x = c.u8(), y = c.u8(), z = c.u8(), index = c.u8();
        if (x < m.sx && y < m.sy && z < m.sz) {
          m.cells[(static_cast<std::size_t>(z) * m.sy + y) * m.sx + x] = index;
        }
      }
    } else if (id == "RGBA") {
      for (std::size_t k = 0; k < 255 && c.remaining() >= 4; ++k) palette[k + 1] = c.u32le();
      custom_palette = true;
    } else if (id == "nTRN" || id == "nGRP" || id == "nSHP") {
      ++scene_nodes;
    }
    CVT_TRY(ctx.checkpoint(0.3 * static_cast<double>(r.position()) / (static_cast<double>(ctx.input.size()) + 1.0)));
  }
  (void)custom_palette;
  if (models.empty()) return fail(Status::Decode, "the MagicaVoxel file contains no models");
  if (scene_nodes > 0 && models.size() > 1) ctx.warn("The MagicaVoxel scene layout was not applied; models are placed side by side.");
  std::array<ir::Color, 256> colors{};
  for (std::size_t k = 0; k < 256; ++k) {
    const std::uint32_t v = palette[k];  // little-endian R, G, B, A bytes
    colors[k] = color_from_srgb(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
  }
  ir::Mesh mesh;
  float offset = 0;
  for (std::size_t m = 0; m < models.size(); ++m) {
    CVT_TRY(ctx.checkpoint(0.3 + 0.6 * static_cast<double>(m) / static_cast<double>(models.size()), "Meshing"));
    const std::size_t first = mesh.face_count();
    greedy_mesh(models[m], colors, offset, mesh);
    CVT_TRY(check_elements(ctx, mesh.positions.size(), "vertices"));
    if (models.size() > 1) mesh.objects.push_back({std::format("model_{}", m), first, mesh.face_count() - first, translation(offset, 0, 0)});
    offset += static_cast<float>(models[m].sx) + 1.0f;
  }
  if (mesh.positions.empty()) return fail(Status::Decode, "the MagicaVoxel model is empty");
  weld(mesh, 0.0f, false, 0.0f);
  mesh.source_up_axis = "Z";
  convert_up_axis(mesh, UpAxis::Z, UpAxis::Y);
  CVT_TRY(finish_read(mesh, ctx));
  return mesh;
}

void add(Registry& registry, std::string id, std::string name, std::string description, std::vector<std::string> extensions,
         std::string mime, int popularity, std::vector<Signature> signatures, std::function<Result<AnyIR>(ReadContext&)> read) {
  FormatInfo info;
  info.id = id;
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Model3D;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.signatures = std::move(signatures);
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = std::move(id);
  codec.kind = Kind::Mesh;
  codec.read = std::move(read);
  registry.add_codec(std::move(codec));
}

}  // namespace

void register_game_readers(Registry& registry) {
  add(registry, "lwo", "LightWave object", "LightWave 3D object (LWO2).", {"lwo"}, "image/x-lwo", 15,
      {Signature::at(0, "'FORM' ?? ?? ?? ?? 'LWO2'")}, read_lwo);
  add(registry, "md2", "Quake II model", "id Software MD2 model (first animation frame).", {"md2"}, "application/x-md2", 8,
      {Signature::at(0, "'IDP2' 08 00 00 00")}, read_md2);
  add(registry, "md3", "Quake III model", "id Software MD3 model (first animation frame).", {"md3"}, "application/x-md3", 8,
      {Signature::at(0, "'IDP3' 0F 00 00 00")}, read_md3);
  add(registry, "ms3d", "MilkShape 3D model", "MilkShape 3D binary model.", {"ms3d"}, "application/x-ms3d", 6,
      {Signature::at(0, "'MS3D000000'")}, read_ms3d);
  add(registry, "x", "DirectX model", "DirectX .x model in text form.", {"x"}, "model/x-directx", 12,
      {Signature::at(0, "'xof 03'")}, read_x);
  add(registry, "vox", "MagicaVoxel model", "Voxel model from MagicaVoxel, converted to a coloured mesh.", {"vox"},
      "application/x-magicavoxel", 15, {Signature::at(0, "'VOX '")}, read_vox);
}

}  // namespace cvt::mesh
