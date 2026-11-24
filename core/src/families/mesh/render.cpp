// Convertible core - mesh family: software preview renderer (Mesh -> Image).
//
// A small deterministic rasteriser: perspective camera framed on the
// model's bounding sphere, z-buffer, perspective-correct interpolation of
// normals, colours and texture coordinates, a three-light rig with
// hemispheric ambient light, shading in linear light with sRGB output, and
// supersampling for anti-aliasing. Point clouds are drawn as round splats.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/mesh/render.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/mesh/common.hpp"
#include "families/mesh/mesh_ops.hpp"
#include "util/text.hpp"

namespace cvt::mesh {

namespace {

struct V3 {
  double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 vcross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 unit(V3 a) {
  const double l = std::sqrt(vdot(a, a));
  return l > 0 && std::isfinite(l) ? a * (1.0 / l) : V3{0, 0, 1};
}
V3 v3(const ir::Vec3& v) { return {v.x, v.y, v.z}; }

// A decoded texture in linear light.
struct TextureImage {
  std::uint32_t width = 0, height = 0;
  std::vector<float> rgba;  // linear rgb, straight alpha

  [[nodiscard]] std::array<float, 4> sample(double u, double v) const {
    // Repeat wrapping, bilinear filtering; v = 0 is the bottom row.
    u = u - std::floor(u);
    v = v - std::floor(v);
    const double x = u * width - 0.5;
    const double y = (1.0 - v) * height - 0.5;
    const auto x0 = static_cast<std::int64_t>(std::floor(x));
    const auto y0 = static_cast<std::int64_t>(std::floor(y));
    const double fx = x - static_cast<double>(x0), fy = y - static_cast<double>(y0);
    const auto texel = [&](std::int64_t tx, std::int64_t ty) {
      const auto w = static_cast<std::int64_t>(width), h = static_cast<std::int64_t>(height);
      tx = ((tx % w) + w) % w;
      ty = ((ty % h) + h) % h;
      const std::size_t at = (static_cast<std::size_t>(ty) * width + static_cast<std::size_t>(tx)) * 4;
      return std::array<float, 4>{rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
    };
    const auto a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
    std::array<float, 4> out{};
    for (int k = 0; k < 4; ++k) {
      out[k] = static_cast<float>((a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy);
    }
    return out;
  }
};

std::optional<TextureImage> decode_texture(const ir::Texture& texture, const Context& ctx) {
  if (texture.data.empty() || ctx.registry == nullptr) return std::nullopt;
  const std::string mime = texture.mime.empty() ? image_mime(texture.data, texture.uri) : texture.mime;
  std::string_view format = image_extension(mime);
  if (format == "bin") return std::nullopt;
  const Codec* reader = nullptr;
  for (const Codec* codec : ctx.registry->codecs_for(format)) {
    if (codec->kind == Kind::Image && codec->read) {
      reader = codec;
      break;
    }
  }
  if (reader == nullptr) return std::nullopt;
  ReadContext read;
  auto options = Options().normalized(reader->read_options);
  if (!options) return std::nullopt;
  read.options = std::move(*options);
  read.limits = ctx.limits;
  read.registry = ctx.registry;
  read.input = texture.data;
  read.format_id = std::string(format);
  auto decoded = reader->read(read);
  if (!decoded || !decoded->is<ir::Image>()) return std::nullopt;
  const ir::Image& image = decoded->get<ir::Image>();
  if (!image.valid()) return std::nullopt;
  const ir::Frame& frame = image.frames.front();
  TextureImage out;
  out.width = frame.width;
  out.height = frame.height;
  out.rgba.resize(frame.rgba.size());
  std::array<float, 256> table{};
  for (int i = 0; i < 256; ++i) table[static_cast<std::size_t>(i)] = srgb_to_linear(static_cast<float>(i) / 255.0f);
  for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
    out.rgba[i] = table[frame.rgba[i]];
    out.rgba[i + 1] = table[frame.rgba[i + 1]];
    out.rgba[i + 2] = table[frame.rgba[i + 2]];
    out.rgba[i + 3] = static_cast<float>(frame.rgba[i + 3]) / 255.0f;
  }
  return out;
}

Result<std::array<float, 4>> parse_background(std::string_view text) {
  text = util::trim(text);
  if (text.empty() || util::iequals(text, "transparent")) return std::array<float, 4>{0, 0, 0, 0};
  // Background colours are given in sRGB; the frame buffer is linear.
  const auto linear = [](float r, float g, float b) {
    return std::array<float, 4>{srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), 1.0f};
  };
  if (util::iequals(text, "light")) return linear(0.93f, 0.94f, 0.96f);
  if (util::iequals(text, "dark")) return linear(0.11f, 0.12f, 0.14f);
  if (text.starts_with('#')) text.remove_prefix(1);
  std::uint32_t rgb = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rgb, 16);
  if (text.size() != 6 || error != std::errc{} || end != text.data() + text.size()) {
    return fail(Status::InvalidArgument, "option 'background' must be transparent, light, dark or RRGGBB");
  }
  return linear(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f);
}

struct Camera {
  V3 eye, right, up, forward;  // forward points from the eye into the scene
  double focal = 1;            // pixels per unit at depth 1
  double cx = 0, cy = 0;       // image centre in pixels
};

// Prepared per-vertex data in camera space.
struct Projected {
  double sx = 0, sy = 0;  // screen position
  double inv_z = 0;       // 1 / depth
  V3 normal;              // camera-space normal
  std::array<float, 4> color{1, 1, 1, 1};
  double u = 0, v = 0;
};

struct Light {
  V3 direction;  // towards the light, camera space
  double intensity;
};

}  // namespace

Result<ir::Image> render_mesh(const ir::Mesh& source, const RenderOptions& options, const Context& ctx) {
  if (source.positions.empty()) return fail(Status::Encode, "the model has no geometry to render");
  const std::uint32_t size = std::clamp<std::uint32_t>(options.size, 8, 8192);
  const std::uint32_t ss = std::clamp<std::uint32_t>(options.supersample, 1, 4);
  const std::uint32_t frames = std::clamp<std::uint32_t>(options.frames, 1, 360);
  CVT_TRY(ctx.limits.check_pixels(size, size, frames));
  CVT_TRY(ctx.limits.check_pixels(std::uint64_t{size} * ss, std::uint64_t{size} * ss));
  CVT_TRY_ASSIGN(const auto background, parse_background(options.background));

  ir::Mesh mesh = source;
  if (!mesh.point_cloud) {
    triangulate(mesh);
    if (mesh.normals.empty()) generate_normals(mesh, 45.0f);
  }
  // Base colour per face from its group's material, textures decoded once.
  std::vector<int> face_material(mesh.face_count(), -1);
  for (const ir::MeshGroup& group : mesh.groups) {
    int index = -1;
    for (std::size_t m = 0; m < mesh.materials.size(); ++m) {
      if (mesh.materials[m].name == group.material) index = static_cast<int>(m);
    }
    for (std::size_t f = group.first_face; f < group.first_face + group.face_count && f < face_material.size(); ++f) {
      face_material[f] = index;
    }
  }
  if (mesh.groups.empty() && mesh.materials.size() == 1) std::fill(face_material.begin(), face_material.end(), 0);
  std::vector<std::optional<TextureImage>> textures(mesh.textures.size());
  for (const ir::Material& m : mesh.materials) {
    if (m.base_color_texture >= 0 && static_cast<std::size_t>(m.base_color_texture) < textures.size() &&
        !textures[m.base_color_texture] && !mesh.texcoords.empty()) {
      textures[m.base_color_texture] = decode_texture(mesh.textures[m.base_color_texture], ctx);
    }
  }

  // Bounding sphere (box centre, farthest vertex).
  const Bounds box = bounds(mesh);
  const V3 center = v3(box.center());
  double radius = 0;
  for (const ir::Vec3& p : mesh.positions) radius = std::max(radius, std::sqrt(vdot(v3(p) - center, v3(p) - center)));
  if (!(radius > 0) || !std::isfinite(radius)) radius = 1;

  double base_yaw = 35, base_pitch = 25;  // iso
  if (options.view == "front") base_yaw = 0, base_pitch = 0;
  else if (options.view == "back") base_yaw = 180, base_pitch = 0;
  else if (options.view == "left") base_yaw = -90, base_pitch = 0;
  else if (options.view == "right") base_yaw = 90, base_pitch = 0;
  else if (options.view == "top") base_yaw = 0, base_pitch = 89.9;
  else if (options.view == "bottom") base_yaw = 0, base_pitch = -89.9;

  const std::uint32_t width = size * ss;
  const double fov = 35.0 * std::numbers::pi / 180.0;
  const double distance = radius / std::sin(fov / 2) * 1.02;

  // Lights in camera space (x right, y up, z towards the viewer).
  const Light lights[3] = {{unit({-0.5, 0.65, 0.6}), 0.85}, {unit({0.7, 0.15, 0.5}), 0.35}, {unit({0.1, 0.5, -0.85}), 0.45}};
  const auto shade = [&](V3 n, V3 view_dir, std::array<float, 4> base, float metallic, float roughness) {
    std::array<float, 4> out{0, 0, 0, base[3]};
    const double hemi = 0.5 + 0.5 * n.y;  // sky above, ground below
    const double ambient = 0.18 * hemi + 0.07 * (1 - hemi);
    const double shininess = 2.0 / std::max(0.02, static_cast<double>(roughness * roughness)) - 2.0;
    const double diffuse_weight = 1.0 - 0.8 * metallic;
    double diffuse = ambient;
    double specular = 0;
    for (const Light& light : lights) {
      const double lambert = std::max(0.0, vdot(n, light.direction));
      diffuse += lambert * light.intensity;
      if (lambert > 0) {
        const V3 half = unit(light.direction + view_dir);
        specular += light.intensity * std::pow(std::max(0.0, vdot(n, half)), std::clamp(shininess, 1.0, 512.0)) *
                    (shininess + 8) / (8 * std::numbers::pi) * 0.25;
      }
    }
    for (int k = 0; k < 3; ++k) {
      const double spec_color = 0.04 * (1 - metallic) + base[k] * metallic;
      out[k] = static_cast<float>(base[k] * diffuse * diffuse_weight + specular * spec_color);
    }
    return out;
  };

  ir::Image image;
  image.loop_count = 0;
  std::vector<float> depth(static_cast<std::size_t>(width) * width);
  std::vector<std::array<float, 4>> color(static_cast<std::size_t>(width) * width);
  std::vector<Projected> projected(mesh.positions.size());
  for (std::uint32_t frame_index = 0; frame_index < frames; ++frame_index) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(frame_index) / frames, "Rendering"));
    const double yaw = (base_yaw + options.yaw + 360.0 * frame_index / frames) * std::numbers::pi / 180.0;
    const double pitch = std::clamp(base_pitch + options.pitch, -89.9, 89.9) * std::numbers::pi / 180.0;
    Camera cam;
    const V3 offset{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
    cam.eye = center + offset * distance;
    cam.forward = unit(center - cam.eye);
    V3 world_up{0, 1, 0};
    if (std::abs(vdot(cam.forward, world_up)) > 0.999) world_up = {0, 0, -1};
    cam.right = unit(vcross(cam.forward, world_up));
    cam.up = vcross(cam.right, cam.forward);
    cam.focal = (width / 2.0) / std::tan(fov / 2);
    cam.cx = cam.cy = width / 2.0;
    if (frames == 1) {
      // Stills: move in until the projected model just fits (5% margin);
      // turntables keep the bounding-sphere distance so frames stay steady.
      const double t = std::tan(fov / 2) * 0.95;
      double fit = 0;
      for (const ir::Vec3& p : mesh.positions) {
        const V3 rel = v3(p) - center;
        const double toward_eye = -vdot(rel, cam.forward);
        fit = std::max(fit, toward_eye + std::max(std::abs(vdot(rel, cam.right)), std::abs(vdot(rel, cam.up))) / t);
      }
      if (fit > 0 && fit < distance) cam.eye = center - cam.forward * fit;
    }

    std::fill(depth.begin(), depth.end(), std::numeric_limits<float>::infinity());
    std::fill(color.begin(), color.end(), background);
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
      const V3 rel = v3(mesh.positions[i]) - cam.eye;
      const double z = std::max(vdot(rel, cam.forward), 1e-6 * distance);
      Projected& p = projected[i];
      p.inv_z = 1.0 / z;
      p.sx = cam.cx + vdot(rel, cam.right) * cam.focal * p.inv_z;
      p.sy = cam.cy - vdot(rel, cam.up) * cam.focal * p.inv_z;
      if (!mesh.normals.empty()) {
        const V3 n = v3(mesh.normals[i]);
        p.normal = {vdot(n, cam.right), vdot(n, cam.up), -vdot(n, cam.forward)};
      }
      if (!mesh.colors.empty()) {
        const ir::Color& c = mesh.colors[i];
        p.color = {c.r, c.g, c.b, c.a};
      } else {
        p.color = {1, 1, 1, 1};
      }
      if (!mesh.texcoords.empty()) {
        p.u = mesh.texcoords[i].x;
        p.v = mesh.texcoords[i].y;
      }
    }

    if (mesh.point_cloud) {
      // Round splats, a little larger for sparse clouds.
      const double splat = std::max(1.5 * ss, width * 1.2 / std::sqrt(static_cast<double>(mesh.positions.size()) + 1.0) * 0.25);
      for (std::size_t i = 0; i < projected.size(); ++i) {
        const Projected& p = projected[i];
        const double r = splat;
        const auto x0 = static_cast<std::int64_t>(std::floor(p.sx - r)), x1 = static_cast<std::int64_t>(std::ceil(p.sx + r));
        const auto y0 = static_cast<std::int64_t>(std::floor(p.sy - r)), y1 = static_cast<std::int64_t>(std::ceil(p.sy + r));
        const auto z = static_cast<float>(1.0 / p.inv_z);
        for (std::int64_t y = std::max<std::int64_t>(y0, 0); y <= std::min<std::int64_t>(y1, width - 1); ++y) {
          for (std::int64_t x = std::max<std::int64_t>(x0, 0); x <= std::min<std::int64_t>(x1, width - 1); ++x) {
            const double dx = static_cast<double>(x) + 0.5 - p.sx, dy = static_cast<double>(y) + 0.5 - p.sy;
            if (dx * dx + dy * dy > r * r) continue;
            const std::size_t at = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
            if (z >= depth[at]) continue;
            depth[at] = z;
            // Sphere-like shading of the splat.
            const double nz = std::sqrt(std::max(0.0, 1 - (dx * dx + dy * dy) / (r * r)));
            color[at] = shade(unit({dx / r, -dy / r, nz}), {0, 0, 1}, p.color, 0.0f, 0.7f);
          }
        }
      }
    } else {
      const std::size_t faces = mesh.indices.size() / 3;
      for (std::size_t f = 0; f < faces; ++f) {
        if ((f & 0x3FFF) == 0) {
          CVT_TRY(ctx.checkpoint((frame_index + 0.9 * static_cast<double>(f) / static_cast<double>(faces)) / frames, "Rendering"));
        }
        const std::uint32_t ia = mesh.indices[f * 3], ib = mesh.indices[f * 3 + 1], ic = mesh.indices[f * 3 + 2];
        const Projected &a = projected[ia], &b = projected[ib], &c = projected[ic];
        const double area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
        if (!(std::abs(area) > 1e-12) || !std::isfinite(area)) continue;
        const double min_x = std::floor(std::min({a.sx, b.sx, c.sx})), max_x = std::ceil(std::max({a.sx, b.sx, c.sx}));
        const double min_y = std::floor(std::min({a.sy, b.sy, c.sy})), max_y = std::ceil(std::max({a.sy, b.sy, c.sy}));
        if (max_x < 0 || max_y < 0 || min_x >= width || min_y >= width) continue;
        const auto x0 = static_cast<std::int64_t>(std::max(min_x, 0.0));
        const auto x1 = static_cast<std::int64_t>(std::min(max_x, width - 1.0));
        const auto y0 = static_cast<std::int64_t>(std::max(min_y, 0.0));
        const auto y1 = static_cast<std::int64_t>(std::min(max_y, width - 1.0));
        const int material = face_material[f];
        std::array<float, 4> factor{1, 1, 1, 1};
        float metallic = 0.0f, roughness = 0.6f;
        const TextureImage* texture = nullptr;
        if (material >= 0) {
          const ir::Material& m = mesh.materials[static_cast<std::size_t>(material)];
          factor = {m.base_color.r, m.base_color.g, m.base_color.b, m.base_color.a};
          metallic = m.metallic;
          roughness = m.roughness;
          if (m.base_color_texture >= 0 && textures[m.base_color_texture]) texture = &*textures[m.base_color_texture];
        }
        const double inv_area = 1.0 / area;
        for (std::int64_t y = y0; y <= y1; ++y) {
          const double py = static_cast<double>(y) + 0.5;
          for (std::int64_t x = x0; x <= x1; ++x) {
            const double px = static_cast<double>(x) + 0.5;
            double w0 = ((b.sx - px) * (c.sy - py) - (b.sy - py) * (c.sx - px)) * inv_area;
            double w1 = ((c.sx - px) * (a.sy - py) - (c.sy - py) * (a.sx - px)) * inv_area;
            double w2 = 1.0 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            // Perspective-correct weights.
            const double iz = w0 * a.inv_z + w1 * b.inv_z + w2 * c.inv_z;
            const auto z = static_cast<float>(1.0 / iz);
            const std::size_t at = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
            if (z >= depth[at]) continue;
            w0 = w0 * a.inv_z / iz;
            w1 = w1 * b.inv_z / iz;
            w2 = 1.0 - w0 - w1;
            std::array<float, 4> base{};
            for (int k = 0; k < 4; ++k) {
              base[k] = static_cast<float>(a.color[k] * w0 + b.color[k] * w1 + c.color[k] * w2) * factor[k];
            }
            if (texture != nullptr) {
              const auto t = texture->sample(a.u * w0 + b.u * w1 + c.u * w2, a.v * w0 + b.v * w1 + c.v * w2);
              for (int k = 0; k < 4; ++k) base[k] *= t[k];
            }
            if (base[3] <= 0.01f) continue;
            depth[at] = z;
            V3 n = unit(a.normal * w0 + b.normal * w1 + c.normal * w2);
            // View direction in camera space for this pixel.
            const V3 view_dir = unit({-(px - cam.cx), (py - cam.cy), cam.focal});
            if (vdot(n, view_dir) < 0) n = n * -1.0;  // previews are two-sided
            color[at] = shade(n, view_dir, base, metallic, roughness);
          }
        }
      }
    }

    // Downsample (box filter over ss x ss) and encode to sRGB.
    ir::Frame out = ir::Frame::filled(size, size);
    for (std::uint32_t y = 0; y < size; ++y) {
      for (std::uint32_t x = 0; x < size; ++x) {
        double r = 0, g = 0, b = 0, alpha = 0;
        for (std::uint32_t sy = 0; sy < ss; ++sy) {
          for (std::uint32_t sx = 0; sx < ss; ++sx) {
            const auto& c = color[static_cast<std::size_t>(y * ss + sy) * width + (x * ss + sx)];
            const double a = std::clamp(c[3], 0.0f, 1.0f);
            r += std::clamp(c[0], 0.0f, 1.0f) * a;
            g += std::clamp(c[1], 0.0f, 1.0f) * a;
            b += std::clamp(c[2], 0.0f, 1.0f) * a;
            alpha += a;
          }
        }
        const std::size_t at = out.offset(x, y);
        const double samples = static_cast<double>(ss) * ss;
        if (alpha > 0) {
          const auto encode = [&](double v) {
            return static_cast<std::uint8_t>(std::lround(linear_to_srgb(static_cast<float>(v / alpha)) * 255.0f));
          };
          out.rgba[at] = encode(r);
          out.rgba[at + 1] = encode(g);
          out.rgba[at + 2] = encode(b);
        }
        out.rgba[at + 3] = static_cast<std::uint8_t>(std::lround(alpha / samples * 255.0));
      }
    }
    out.delay_ms = frames > 1 ? options.frame_delay_ms : 0;
    image.frames.push_back(std::move(out));
  }
  return image;
}

}  // namespace cvt::mesh
