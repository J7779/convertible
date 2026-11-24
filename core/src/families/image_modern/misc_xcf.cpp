// Convertible core - image_modern family: GIMP XCF (read).
//
// Versions 0 to 12 and later (header "gimp xcf file" / "gimp xcf vNNN"),
// 32-bit offsets before version 11 and 64-bit offsets from it. Tiles stored
// uncompressed, RLE or zlib. Precisions: 8/16/32-bit integer, half, float
// and double, linear or gamma/perceptual; RGB, grey and indexed, each with
// or without alpha. The visible layers are composited bottom to top with
// their offsets, opacity, layer masks and layer groups; every blend mode
// is composited as "normal" (with a warning when others are present), in
// linear light for GIMP 2.10+ modes and on sRGB-encoded values for legacy
// modes and perceptual composite spaces, as GIMP does. Text layers,
// channels, paths and the selection are ignored (text layers are shown as
// their rendered pixels).
// High-precision documents also fill `ir::Image::hdr` (linear light).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/misc.hpp"
#include "families/image_modern/misc_pixel.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"

namespace cvt::image_modern {

namespace {

using image::ImageCodecSpec;

// Property ids (GIMP's xcf-private.h).
enum : std::uint32_t {
  prop_end = 0,
  prop_colormap = 1,
  prop_floating_selection = 5,
  prop_opacity = 6,
  prop_mode = 7,
  prop_visible = 8,
  prop_apply_mask = 11,
  prop_offsets = 15,
  prop_compression = 17,
  prop_resolution = 19,
  prop_parasites = 21,
  prop_group_item = 29,
  prop_item_path = 30,
  prop_float_opacity = 33,
  prop_composite_space = 36,
};

enum class Component { U8, U16, U32, Half, Float, Double };

constexpr int tile_size = 64;
constexpr std::size_t max_properties = 100000;
constexpr int max_group_depth = 16;

struct Precision {
  Component component = Component::U8;
  bool linear = false;
  [[nodiscard]] int bytes() const noexcept {
    switch (component) {
      case Component::U8: return 1;
      case Component::U16: case Component::Half: return 2;
      case Component::U32: case Component::Float: return 4;
      case Component::Double: return 8;
    }
    return 1;
  }
};

struct LayerInfo {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t type = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  double opacity = 1.0;
  bool visible = true;
  bool apply_mask = true;
  bool group = false;
  bool floating = false;
  std::uint32_t mode = 0;
  std::int32_t composite_space = 0;
  std::vector<std::uint32_t> path;
  std::uint64_t hierarchy = 0;
  std::uint64_t mask = 0;
  std::vector<std::size_t> children;  // indices into the layer list, top-most first
};

/// One decoded hierarchy level: tile offsets and geometry.
struct Level {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t bpp = 0;
  std::vector<std::uint64_t> tiles;
};

/// Source-over of one pixel. `s` is linear-light colour; `sa` its effective
/// alpha. The canvas pixel `d` holds linear colour when `canvas_linear`, else
/// sRGB-encoded colour; the blend itself happens in linear light or on encoded
/// values as `layer_linear` says.
inline void blend(float* d, const float* s, float sa, bool layer_linear, bool canvas_linear) {
  if (sa <= 0.0f) return;
  const float da = d[3];
  const float out_a = sa + da * (1.0f - sa);
  for (int c = 0; c < 3; ++c) {
    const float dv = layer_linear == canvas_linear ? d[c]
                     : canvas_linear              ? image::linear_to_srgb(d[c])
                                                  : image::srgb_to_linear(d[c]);
    const float sv = layer_linear ? s[c] : image::linear_to_srgb(s[c]);
    const float v = (sv * sa + dv * da * (1.0f - sa)) / out_a;
    d[c] = layer_linear == canvas_linear ? v : (canvas_linear ? image::srgb_to_linear(v) : image::linear_to_srgb(v));
  }
  d[3] = out_a;
}

class XcfFile {
 public:
  XcfFile(ByteView data, const ReadContext& ctx) : data_(data), ctx_(ctx) {}

  Result<ir::Image> read();

 private:
  Result<void> parse_header();
  Result<void> parse_properties(util::ByteReader& r, LayerInfo* layer, bool image_level);
  Result<std::uint64_t> pointer(util::ByteReader& r) {
    const std::uint64_t value = wide_ ? r.u64be() : r.u32be();
    if (!r.ok()) return fail(Status::Decode, "XCF file is truncated");
    return value;
  }
  Result<std::string> string(util::ByteReader& r) {
    const std::uint32_t length = r.u32be();
    if (!r.ok() || length > r.remaining()) return fail(Status::Decode, "XCF string is truncated");
    std::string text(r.chars(length));
    if (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
  }
  Result<LayerInfo> parse_layer(std::uint64_t offset);
  Result<Level> parse_hierarchy(std::uint64_t offset, std::uint32_t width, std::uint32_t height);
  Result<void> decode_tile(const Level& level, std::size_t index, std::uint32_t tw, std::uint32_t th,
                           std::vector<std::uint8_t>& out);
  Result<void> render(std::size_t index, std::vector<float>& canvas, bool canvas_linear, int depth);
  Result<void> composite_layer(const LayerInfo& layer, std::vector<float>& canvas, bool canvas_linear);
  [[nodiscard]] bool layer_linear_composite(const LayerInfo& layer) const noexcept;
  void to_float(const std::uint8_t* px, std::uint32_t type, float* out) const noexcept;
  [[nodiscard]] float component(const std::uint8_t* p) const noexcept;

  ByteView data_;
  const ReadContext& ctx_;
  int version_ = 0;
  bool wide_ = false;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t base_type_ = 0;
  Precision precision_;
  int compression_ = 0;
  std::vector<std::array<std::uint8_t, 3>> colormap_;
  double dpi_x_ = 0.0;
  double dpi_y_ = 0.0;
  Bytes icc_;
  std::string comment_;
  std::vector<LayerInfo> layers_;
  std::uint64_t decoded_pixels_ = 0;
  std::uint64_t layers_done_ = 0;
  bool warned_mode_ = false;
};

Result<void> XcfFile::parse_header() {
  util::ByteReader r(data_);
  if (!r.consume("gimp xcf ")) return fail(Status::Decode, "not a GIMP XCF file");
  const std::string_view tag = r.chars(4);
  if (!r.ok()) return fail(Status::Decode, "XCF header is truncated");
  if (tag == "file") {
    version_ = 0;
  } else if (tag[0] == 'v' && tag[1] >= '0' && tag[1] <= '9' && tag[2] >= '0' && tag[2] <= '9' && tag[3] >= '0' &&
             tag[3] <= '9') {
    version_ = (tag[1] - '0') * 100 + (tag[2] - '0') * 10 + (tag[3] - '0');
  } else {
    return fail(Status::Decode, "unknown XCF version tag");
  }
  if (r.u8() != 0) return fail(Status::Decode, "XCF header is malformed");
  wide_ = version_ >= 11;
  width_ = r.u32be();
  height_ = r.u32be();
  base_type_ = r.u32be();
  if (version_ >= 4) {
    const std::uint32_t p = r.u32be();
    if (version_ == 4) {
      switch (p) {
        case 0: precision_ = {Component::U8, false}; break;
        case 1: precision_ = {Component::U16, false}; break;
        case 2: precision_ = {Component::U32, true}; break;
        case 3: precision_ = {Component::Half, true}; break;
        case 4: precision_ = {Component::Float, true}; break;
        default: return fail(Status::Decode, "unknown XCF precision {}", p);
      }
    } else if (version_ <= 6) {
      switch (p) {
        case 100: precision_ = {Component::U8, true}; break;
        case 150: precision_ = {Component::U8, false}; break;
        case 200: precision_ = {Component::U16, true}; break;
        case 250: precision_ = {Component::U16, false}; break;
        case 300: precision_ = {Component::U32, true}; break;
        case 350: precision_ = {Component::U32, false}; break;
        case 400: precision_ = {Component::Half, true}; break;
        case 450: precision_ = {Component::Half, false}; break;
        case 500: precision_ = {Component::Float, true}; break;
        case 550: precision_ = {Component::Float, false}; break;
        default: return fail(Status::Decode, "unknown XCF precision {}", p);
      }
    } else {
      // GimpPrecision: x00 linear, x50 non-linear ("gamma"), x75 perceptual.
      const std::uint32_t kind = p / 100;
      const std::uint32_t curve = p % 100;
      if (curve != 0 && curve != 50 && curve != 75) return fail(Status::Decode, "unknown XCF precision {}", p);
      precision_.linear = curve == 0;
      switch (kind) {
        case 1: precision_.component = Component::U8; break;
        case 2: precision_.component = Component::U16; break;
        case 3: precision_.component = Component::U32; break;
        case 5: precision_.component = Component::Half; break;
        case 6: precision_.component = Component::Float; break;
        case 7: precision_.component = Component::Double; break;
        default: return fail(Status::Decode, "unknown XCF precision {}", p);
      }
    }
  }
  if (!r.ok()) return fail(Status::Decode, "XCF header is truncated");
  if (base_type_ > 2) return fail(Status::Decode, "unknown XCF image type {}", base_type_);
  if (base_type_ == 2 && precision_.component != Component::U8) {
    return fail(Status::Decode, "indexed XCF images must be 8-bit");
  }
  CVT_TRY(image::check_size(ctx_, width_, height_));
  // The composite is kept as floats: refuse gigapixel canvases declared by a few bytes.
  CVT_TRY(image::check_plausible(std::uint64_t{width_} * height_, data_.size(), 65536));
  if (std::uint64_t{width_} * height_ * 16 > std::numeric_limits<std::size_t>::max() / 2) {
    return fail(Status::Limit, "the XCF canvas is too large for this platform");
  }

  CVT_TRY(parse_properties(r, nullptr, true));
  std::vector<std::uint64_t> layer_offsets;
  while (true) {
    CVT_TRY_ASSIGN(const std::uint64_t offset, pointer(r));
    if (offset == 0) break;
    CVT_TRY(ctx_.limits.check_archive_entries(layer_offsets.size() + 1));
    layer_offsets.push_back(offset);
  }
  for (const std::uint64_t offset : layer_offsets) {
    CVT_TRY_ASSIGN(LayerInfo layer, parse_layer(offset));
    layers_.push_back(std::move(layer));
  }
  return {};
}

Result<void> XcfFile::parse_properties(util::ByteReader& r, LayerInfo* layer, bool image_level) {
  for (std::size_t count = 0;; ++count) {
    if (count > max_properties) return fail(Status::Decode, "XCF property list is too long");
    const std::uint32_t id = r.u32be();
    const std::uint32_t size = r.u32be();
    if (!r.ok()) return fail(Status::Decode, "XCF property list is truncated");
    if (id == prop_end) return {};
    if (size > r.remaining()) return fail(Status::Decode, "XCF property is truncated");
    util::ByteReader p(r.bytes(size));
    if (image_level) {
      switch (id) {
        case prop_colormap: {
          const std::uint32_t n = p.u32be();
          if (n > 256 || p.remaining() < std::size_t{n} * 3) {
            // Version 0 files may store a bogus count; take what is there.
            if (version_ != 0 || n > 256) return fail(Status::Decode, "XCF colour map is invalid");
          }
          colormap_.clear();
          for (std::uint32_t i = 0; i < n && p.remaining() >= 3; ++i) {
            const std::uint8_t cr = p.u8();
            const std::uint8_t cg = p.u8();
            const std::uint8_t cb = p.u8();
            colormap_.push_back({cr, cg, cb});
          }
          break;
        }
        case prop_compression: compression_ = p.u8(); break;
        case prop_resolution: {
          const float x = p.f32be();
          const float y = p.f32be();
          if (p.ok() && std::isfinite(x) && std::isfinite(y) && x > 0 && y > 0 && x < 1e6f && y < 1e6f) {
            dpi_x_ = x;
            dpi_y_ = y;
          }
          break;
        }
        case prop_parasites:
          while (p.remaining() > 0) {
            CVT_TRY_ASSIGN(const std::string name, string(p));
            (void)p.u32be();  // flags
            const std::uint32_t length = p.u32be();
            if (!p.ok() || length > p.remaining()) break;
            const ByteView value = p.bytes(length);
            if (name == "icc-profile") icc_ = to_bytes(value);
            if (name == "gimp-comment") {
              comment_ = std::string(as_chars(value));
              while (!comment_.empty() && comment_.back() == '\0') comment_.pop_back();
            }
          }
          break;
        default: break;
      }
      continue;
    }
    switch (id) {
      case prop_opacity: layer->opacity = std::min(255u, p.u32be()) / 255.0; break;
      case prop_float_opacity: {
        const float v = p.f32be();
        if (p.ok() && std::isfinite(v)) layer->opacity = std::clamp(static_cast<double>(v), 0.0, 1.0);
        break;
      }
      case prop_visible: layer->visible = p.u32be() != 0; break;
      case prop_apply_mask: layer->apply_mask = p.u32be() != 0; break;
      case prop_offsets:
        layer->x = p.i32be();
        layer->y = p.i32be();
        break;
      case prop_mode: layer->mode = p.u32be(); break;
      case prop_group_item: layer->group = true; break;
      case prop_floating_selection: layer->floating = true; break;
      case prop_composite_space: layer->composite_space = p.i32be(); break;
      case prop_item_path:
        layer->path.clear();
        while (p.remaining() >= 4 && layer->path.size() < static_cast<std::size_t>(max_group_depth) + 1) {
          layer->path.push_back(p.u32be());
        }
        break;
      default: break;
    }
  }
}

Result<LayerInfo> XcfFile::parse_layer(std::uint64_t offset) {
  if (offset >= data_.size()) return fail(Status::Decode, "XCF layer offset is out of range");
  util::ByteReader r(data_);
  (void)r.seek(static_cast<std::size_t>(offset));
  LayerInfo layer;
  layer.width = r.u32be();
  layer.height = r.u32be();
  layer.type = r.u32be();
  if (!r.ok()) return fail(Status::Decode, "XCF layer is truncated");
  CVT_TRY_ASSIGN(const std::string name, string(r));
  (void)name;
  CVT_TRY(parse_properties(r, &layer, false));
  CVT_TRY_ASSIGN(layer.hierarchy, pointer(r));
  CVT_TRY_ASSIGN(layer.mask, pointer(r));
  if (layer.type > 5) return fail(Status::Decode, "unknown XCF layer type {}", layer.type);
  return layer;
}

Result<Level> XcfFile::parse_hierarchy(std::uint64_t offset, std::uint32_t width, std::uint32_t height) {
  if (offset == 0 || offset >= data_.size()) return fail(Status::Decode, "XCF pixel data offset is out of range");
  util::ByteReader r(data_);
  (void)r.seek(static_cast<std::size_t>(offset));
  Level level;
  const std::uint32_t hw = r.u32be();
  const std::uint32_t hh = r.u32be();
  level.bpp = r.u32be();
  CVT_TRY_ASSIGN(const std::uint64_t level_offset, pointer(r));
  if (hw != width || hh != height) return fail(Status::Decode, "XCF pixel data size does not match its layer");
  if (level_offset == 0 || level_offset >= data_.size()) return fail(Status::Decode, "XCF level offset is out of range");
  (void)r.seek(static_cast<std::size_t>(level_offset));
  level.width = r.u32be();
  level.height = r.u32be();
  if (!r.ok() || level.width != width || level.height != height) {
    return fail(Status::Decode, "XCF level size does not match its layer");
  }
  const std::uint64_t count = (std::uint64_t{width} + tile_size - 1) / tile_size *
                              ((std::uint64_t{height} + tile_size - 1) / tile_size);
  // Every tile needs at least a pointer: a cheap plausibility check before allocating.
  if (count > r.remaining() / (wide_ ? 8 : 4)) return fail(Status::Decode, "XCF tile table is truncated");
  level.tiles.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    CVT_TRY_ASSIGN(const std::uint64_t tile, pointer(r));
    if (tile == 0 || tile >= data_.size()) return fail(Status::Decode, "XCF tile offset is out of range");
    level.tiles.push_back(tile);
  }
  // The table is followed by a terminating zero (and, for the last tile, its end).
  CVT_TRY_ASSIGN(const std::uint64_t end, pointer(r));
  level.tiles.push_back(end);
  return level;
}

Result<void> XcfFile::decode_tile(const Level& level, std::size_t index, std::uint32_t tw, std::uint32_t th,
                                  std::vector<std::uint8_t>& out) {
  const std::size_t pixels = std::size_t{tw} * th;
  const std::size_t bpp = level.bpp;
  const std::size_t size = pixels * bpp;
  out.assign(size, 0);
  const std::size_t start = static_cast<std::size_t>(level.tiles[index]);
  // Data runs to the next tile, or to the end of the file for the last one.
  std::size_t end = data_.size();
  const std::uint64_t next = level.tiles[index + 1];
  if (next > start && next <= data_.size()) end = static_cast<std::size_t>(next);
  const auto* src = reinterpret_cast<const std::uint8_t*>(data_.data());
  switch (compression_) {
    case 0: {
      if (end - start < size) return fail(Status::Decode, "XCF tile is truncated");
      std::copy_n(src + start, size, out.data());
      return {};
    }
    case 1: {
      std::size_t pos = start;
      for (std::size_t plane = 0; plane < bpp; ++plane) {
        std::size_t done = 0;
        while (done < pixels) {
          if (pos >= end) return fail(Status::Decode, "XCF RLE tile is truncated");
          std::size_t length = src[pos++];
          if (length >= 128) {
            length = 256 - length;
            if (length == 128) {
              if (pos + 2 > end) return fail(Status::Decode, "XCF RLE tile is truncated");
              length = std::size_t{src[pos]} << 8 | src[pos + 1];
              pos += 2;
            }
            if (length > pixels - done || pos + length > end) return fail(Status::Decode, "XCF RLE tile is corrupt");
            for (std::size_t i = 0; i < length; ++i) out[(done + i) * bpp + plane] = src[pos + i];
            pos += length;
          } else {
            length += 1;
            if (length == 128) {
              if (pos + 2 > end) return fail(Status::Decode, "XCF RLE tile is truncated");
              length = std::size_t{src[pos]} << 8 | src[pos + 1];
              pos += 2;
            }
            if (pos >= end || length > pixels - done) return fail(Status::Decode, "XCF RLE tile is corrupt");
            const std::uint8_t value = src[pos++];
            for (std::size_t i = 0; i < length; ++i) out[(done + i) * bpp + plane] = value;
          }
          done += length;
        }
      }
      return {};
    }
    case 2: {
      CVT_TRY_ASSIGN(const Bytes inflated, util::zlib_decompress(data_.subspan(start, end - start), size));
      if (inflated.size() != size) return fail(Status::Decode, "XCF zlib tile has the wrong size");
      std::copy_n(reinterpret_cast<const std::uint8_t*>(inflated.data()), size, out.data());
      return {};
    }
    default: return fail(Status::UnsupportedConversion, "XCF compression {} is not supported", compression_);
  }
}

float XcfFile::component(const std::uint8_t* p) const noexcept {
  switch (precision_.component) {
    case Component::U8: return p[0] / 255.0f;
    case Component::U16: return load_be16(p) / 65535.0f;
    case Component::U32: return static_cast<float>(load_be32(p) / 4294967295.0);
    // Floats are bounded so compositing can never produce infinities or NaN.
    case Component::Half: return std::clamp(finite_or_zero(half_to_float(load_be16(p))), -65504.0f, 65504.0f);
    case Component::Float: return std::clamp(finite_or_zero(std::bit_cast<float>(load_be32(p))), -65504.0f, 65504.0f);
    case Component::Double: {
      const double v = std::bit_cast<double>(load_be64(p));
      return std::isfinite(v) ? static_cast<float>(std::clamp(v, -65504.0, 65504.0)) : 0.0f;
    }
  }
  return 0.0f;
}

/// One pixel of a layer -> linear-light RGBA float (straight alpha).
void XcfFile::to_float(const std::uint8_t* px, std::uint32_t type, float* out) const noexcept {
  const int cb = precision_.bytes();
  const auto colour = [&](float v) { return precision_.linear ? v : image::srgb_to_linear(v); };
  switch (type) {
    case 0:
    case 1:
      for (int c = 0; c < 3; ++c) out[c] = colour(component(px + c * cb));
      out[3] = type == 1 ? std::clamp(component(px + 3 * cb), 0.0f, 1.0f) : 1.0f;
      break;
    case 2:
    case 3: {
      const float g = colour(component(px));
      out[0] = out[1] = out[2] = g;
      out[3] = type == 3 ? std::clamp(component(px + cb), 0.0f, 1.0f) : 1.0f;
      break;
    }
    default: {
      const std::size_t index = px[0];
      if (index < colormap_.size()) {
        for (int c = 0; c < 3; ++c) out[c] = image::srgb_to_linear(colormap_[index][static_cast<std::size_t>(c)]);
      } else {
        out[0] = out[1] = out[2] = 0.0f;
      }
      out[3] = type == 5 ? px[1] / 255.0f : 1.0f;
      break;
    }
  }
}

bool XcfFile::layer_linear_composite(const LayerInfo& layer) const noexcept {
  // Legacy modes (GIMP < 2.10 and their "legacy" variants) blend perceptually.
  if (layer.mode <= 22) return false;
  const std::int32_t space = layer.composite_space < 0 ? -layer.composite_space : layer.composite_space;
  if (space == 2 || space == 4) return false;  // RGB non-linear / perceptual
  return true;                                  // auto and RGB linear
}

Result<void> XcfFile::composite_layer(const LayerInfo& layer, std::vector<float>& canvas, bool canvas_linear) {
  const std::uint32_t expected_channels =
      layer.type <= 1 ? 3 + layer.type : (layer.type <= 3 ? 1 + (layer.type - 2) : 1 + (layer.type - 4));
  const std::uint32_t component_bytes = layer.type >= 4 ? 1 : static_cast<std::uint32_t>(precision_.bytes());
  if (layer.type >= 4 && colormap_.empty()) return fail(Status::Decode, "indexed XCF layer without a colour map");
  CVT_TRY_ASSIGN(const Level level, parse_hierarchy(layer.hierarchy, layer.width, layer.height));
  if (level.bpp != expected_channels * component_bytes) {
    return fail(Status::Decode, "XCF layer has {} bytes per pixel, expected {}", level.bpp,
                expected_channels * component_bytes);
  }
  std::optional<Level> mask;
  if (layer.mask != 0 && layer.apply_mask) {
    // Channel header: width, height, name, properties, hierarchy pointer.
    if (layer.mask >= data_.size()) return fail(Status::Decode, "XCF mask offset is out of range");
    util::ByteReader r(data_);
    (void)r.seek(static_cast<std::size_t>(layer.mask));
    const std::uint32_t mw = r.u32be();
    const std::uint32_t mh = r.u32be();
    CVT_TRY_ASSIGN(const std::string name, string(r));
    (void)name;
    LayerInfo ignored;
    CVT_TRY(parse_properties(r, &ignored, false));
    CVT_TRY_ASSIGN(const std::uint64_t hierarchy, pointer(r));
    if (mw == layer.width && mh == layer.height) {
      CVT_TRY_ASSIGN(Level m, parse_hierarchy(hierarchy, mw, mh));
      if (m.bpp == static_cast<std::uint32_t>(precision_.bytes())) mask = std::move(m);
    }
    if (!mask) ctx_.warn("An XCF layer mask could not be used and was ignored.");
  }

  const bool layer_linear = layer_linear_composite(layer);
  const auto tiles_x = (layer.width + tile_size - 1) / tile_size;
  const auto tiles_y = (layer.height + tile_size - 1) / tile_size;
  std::vector<std::uint8_t> tile;
  std::vector<std::uint8_t> mask_tile;
  for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
    for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
      const std::size_t index = std::size_t{ty} * tiles_x + tx;
      const std::uint32_t tw = std::min<std::uint32_t>(tile_size, layer.width - tx * tile_size);
      const std::uint32_t th = std::min<std::uint32_t>(tile_size, layer.height - ty * tile_size);
      // Skip tiles entirely outside the canvas.
      const std::int64_t left = std::int64_t{layer.x} + std::int64_t{tx} * tile_size;
      const std::int64_t top = std::int64_t{layer.y} + std::int64_t{ty} * tile_size;
      if (left >= width_ || top >= height_ || left + tw <= 0 || top + th <= 0) continue;
      CVT_TRY(decode_tile(level, index, tw, th, tile));
      if (mask) CVT_TRY(decode_tile(*mask, index, tw, th, mask_tile));
      for (std::uint32_t y = 0; y < th; ++y) {
        const std::int64_t cy = top + y;
        if (cy < 0 || cy >= height_) continue;
        for (std::uint32_t x = 0; x < tw; ++x) {
          const std::int64_t cx = left + x;
          if (cx < 0 || cx >= width_) continue;
          const std::size_t i = std::size_t{y} * tw + x;
          float s[4];
          to_float(tile.data() + i * level.bpp, layer.type, s);
          if (mask) {
            const float m = std::clamp(component(mask_tile.data() + i * mask->bpp), 0.0f, 1.0f);
            s[3] *= m;
          }
          float* d = canvas.data() + (static_cast<std::size_t>(cy) * width_ + static_cast<std::size_t>(cx)) * 4;
          blend(d, s, s[3] * static_cast<float>(layer.opacity), layer_linear, canvas_linear);
        }
      }
    }
    CVT_TRY(ctx_.checkpoint(std::min(0.99, (static_cast<double>(layers_done_) + static_cast<double>(ty + 1) / tiles_y) /
                                               static_cast<double>(layers_.size() + 1))));
  }
  return {};
}

Result<void> XcfFile::render(std::size_t index, std::vector<float>& canvas, bool canvas_linear, int depth) {
  if (depth > max_group_depth) return fail(Status::Limit, "XCF layer groups are nested too deeply");
  const LayerInfo& layer = layers_[index];
  if (!layer.visible) return {};
  if (layer.mode != 0 && layer.mode != 28 && !(layer.group && layer.mode == 61) && !warned_mode_) {
    ctx_.warn("XCF blend modes other than normal are composited as normal.");
    warned_mode_ = true;
  }
  decoded_pixels_ += std::uint64_t{layer.width} * layer.height;
  if (decoded_pixels_ / 16 > ctx_.limits.max_pixels) return fail(Status::Limit, "the XCF layers are too large");
  if (!layer.group) {
    CVT_TRY(composite_layer(layer, canvas, canvas_linear));
    ++layers_done_;
    return {};
  }
  // Group: render the children into their own buffer, then composite that
  // buffer with the group's opacity (masks on groups are not applied).
  std::vector<float> group(canvas.size(), 0.0f);
  for (auto it = layer.children.rbegin(); it != layer.children.rend(); ++it) {
    CVT_TRY(render(*it, group, canvas_linear, depth + 1));
  }
  const float opacity = static_cast<float>(layer.opacity);
  const bool group_linear = layer_linear_composite(layer);
  for (std::size_t i = 0; i < canvas.size(); i += 4) {
    float src[4] = {group[i], group[i + 1], group[i + 2], group[i + 3]};
    if (!canvas_linear) {
      for (int c = 0; c < 3; ++c) src[c] = image::srgb_to_linear(src[c]);
    }
    blend(canvas.data() + i, src, src[3] * opacity, group_linear, canvas_linear);
  }
  return {};
}

Result<ir::Image> XcfFile::read() {
  CVT_TRY(parse_header());
  // Build the group tree from the item paths (file order is depth first,
  // top-most first).
  std::vector<std::size_t> roots;
  std::uint32_t top_level = 0;
  for (LayerInfo& layer : layers_) {
    // Top-level layers may carry no path: their path is their position.
    if (layer.path.empty()) layer.path.push_back(top_level);
    if (layer.path.size() == 1) top_level = layer.path[0] + 1;
  }
  for (std::size_t i = 0; i < layers_.size(); ++i) {
    LayerInfo& layer = layers_[i];
    if (layer.floating) continue;
    if (layer.path.size() <= 1) {
      roots.push_back(i);
      continue;
    }
    // The parent is the closest earlier layer whose path is this one's prefix.
    bool attached = false;
    for (std::size_t j = i; j-- > 0;) {
      const LayerInfo& candidate = layers_[j];
      if (candidate.group && candidate.path.size() + 1 == layer.path.size() &&
          std::equal(candidate.path.begin(), candidate.path.end(), layer.path.begin())) {
        layers_[j].children.push_back(i);
        attached = true;
        break;
      }
    }
    if (!attached) roots.push_back(i);
  }
  // Composite in linear light when any layer asks for it, else on encoded values.
  bool linear = false;
  for (const LayerInfo& layer : layers_) linear = linear || (!layer.group && layer_linear_composite(layer));

  std::vector<float> canvas(std::size_t{width_} * height_ * 4, 0.0f);
  for (auto it = roots.rbegin(); it != roots.rend(); ++it) CVT_TRY(render(*it, canvas, linear, 0));

  ir::Image image;
  ir::Frame frame = ir::Frame::filled(width_, height_);
  const bool high_precision = precision_.component != Component::U8;
  ir::HdrPixels hdr;
  if (high_precision) {
    hdr.width = width_;
    hdr.height = height_;
    hdr.rgba.resize(canvas.size());
  }
  for (std::size_t i = 0; i < canvas.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      const float v = canvas[i + static_cast<std::size_t>(c)];
      const float lin = linear ? v : image::srgb_to_linear(v);
      frame.rgba[i + static_cast<std::size_t>(c)] = linear ? image::linear_to_srgb8(v)
                                                           : static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
      if (high_precision) hdr.rgba[i + static_cast<std::size_t>(c)] = lin;
    }
    const float a = std::clamp(canvas[i + 3], 0.0f, 1.0f);
    frame.rgba[i + 3] = static_cast<std::uint8_t>(std::lround(a * 255.0f));
    if (high_precision) hdr.rgba[i + 3] = a;
  }
  image.frames.push_back(std::move(frame));
  if (high_precision) {
    image.hdr = std::move(hdr);
    image.source_bit_depth = precision_.component == Component::U16 || precision_.component == Component::U32 ? 16 : 32;
  }
  image.source_grayscale = base_type_ == 1;
  image.dpi_x = dpi_x_;
  image.dpi_y = dpi_y_;
  image.icc_profile = std::move(icc_);
  if (!comment_.empty()) image.metadata["Comment"] = comment_;
  return image;
}

Result<ir::Image> read_xcf(ReadContext& ctx) {
  XcfFile file(ctx.input, ctx);
  return file.read();
}

}  // namespace

void register_misc_xcf(Registry& registry) {
  FormatInfo info;
  info.id = "xcf";
  info.name = "GIMP image";
  info.description = "GIMP's native layered document (the visible layers are flattened).";
  info.category = Category::Image;
  info.extensions = {"xcf"};
  info.mime = "image/x-xcf";
  info.popularity = 25;
  info.signatures = {Signature::at(0, "'gimp xcf '")};
  registry.add_format(std::move(info));

  ImageCodecSpec codec;
  codec.format_id = "xcf";
  codec.read = image::reader(read_xcf);
  image::add_image_codec(registry, std::move(codec));
}

}  // namespace cvt::image_modern
