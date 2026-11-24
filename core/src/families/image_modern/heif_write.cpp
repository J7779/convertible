// Convertible core - image_modern family: AVIF and HEIC/HEIF writing.
//
// One primary image item (AV1 via codec_av1's encode_still, HEVC via
// codec_hevc's encode_picture; HEVC pictures beyond the level 6.2 limits are
// split into a 512x512 'grid'), an optional alpha auxiliary item, and, only
// when strip_metadata is false, an ICC 'colr', an 'Exif' and an XMP item.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codec/colorspace.hpp"
#include "engine/standard_options.hpp"
#include "families/codec_av1/av1_api.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/heif_internal.hpp"

namespace cvt::image_modern::heif {

namespace {

constexpr std::uint32_t hevc_tile_size = 512;

// ------------------------------------------------------------------------------ file model

struct OutItem {
  std::uint32_t id = 0;
  std::string type;
  bool hidden = false;
  std::string content_type;
  Bytes data;
  std::vector<std::pair<std::uint16_t, bool>> properties;  // 1-based index, essential
};

struct OutRef {
  std::string type;
  std::uint32_t from = 0;
  std::vector<std::uint32_t> to;
};

class FileBuilder {
 public:
  std::uint32_t add_item(std::string type, Bytes data, bool hidden = false) {
    OutItem item;
    item.id = static_cast<std::uint32_t>(items_.size() + 1);
    item.type = std::move(type);
    item.data = std::move(data);
    item.hidden = hidden;
    items_.push_back(std::move(item));
    return items_.back().id;
  }
  OutItem& item(std::uint32_t id) { return items_[id - 1]; }
  /// Associate a property; identical boxes are shared unless `own` is set.
  void link(std::uint32_t id, Bytes property_box, bool essential, bool own = false) {
    std::uint16_t index = 0;
    for (std::size_t i = 0; i < properties_.size() && !own; ++i) {
      if (properties_[i] == property_box) index = static_cast<std::uint16_t>(i + 1);
    }
    if (index == 0) {
      properties_.push_back(std::move(property_box));
      index = static_cast<std::uint16_t>(properties_.size());
    }
    item(id).properties.emplace_back(index, essential);
  }
  void reference(std::string type, std::uint32_t from, std::vector<std::uint32_t> to) {
    refs_.push_back({std::move(type), from, std::move(to)});
  }

  Result<Bytes> build(std::string_view major, const std::vector<std::string_view>& brands, std::uint32_t primary) {
    BoxWriter ftyp;
    const std::size_t f = ftyp.begin("ftyp");
    ftyp.out().text(major).u32be(0);
    for (const std::string_view brand : brands) ftyp.out().text(brand);
    CVT_TRY(ftyp.end(f));
    const Bytes ftyp_bytes = ftyp.take();

    std::uint64_t payload = 0;
    for (const OutItem& item : items_) payload += item.data.size();
    // The meta box has the same size whatever the offsets are: build it once
    // to measure, then again with the real offsets.
    CVT_TRY_ASSIGN(const Bytes probe, build_meta(primary, 0));
    const std::uint64_t mdat_header = payload + 8 > 0xFFFFFFFFu ? 16 : 8;
    const std::uint64_t data_start = ftyp_bytes.size() + probe.size() + mdat_header;
    if (data_start + payload > 0xFFFFFFFFu) return fail(Status::Limit, "the HEIF file would exceed 4 GiB");
    CVT_TRY_ASSIGN(const Bytes meta, build_meta(primary, static_cast<std::uint32_t>(data_start)));

    util::ByteWriter out(static_cast<std::size_t>(data_start + payload));
    out.bytes(ftyp_bytes).bytes(meta);
    if (mdat_header == 16) {
      out.u32be(1).text("mdat").u64be(payload + 16);
    } else {
      out.u32be(static_cast<std::uint32_t>(payload + 8)).text("mdat");
    }
    for (const OutItem& item : items_) out.bytes(item.data);
    return out.take();
  }

 private:
  Result<Bytes> build_meta(std::uint32_t primary, std::uint32_t data_start) const {
    BoxWriter w;
    const std::size_t meta = w.begin_full("meta", 0, 0);

    const std::size_t hdlr = w.begin_full("hdlr", 0, 0);
    w.out().u32be(0).text("pict").u32be(0).u32be(0).u32be(0).u8(0);
    CVT_TRY(w.end(hdlr));

    const std::size_t pitm = w.begin_full("pitm", 0, 0);
    w.out().u16be(static_cast<std::uint16_t>(primary));
    CVT_TRY(w.end(pitm));

    const std::size_t iloc = w.begin_full("iloc", 0, 0);
    w.out().u8(0x44).u8(0x00).u16be(static_cast<std::uint16_t>(items_.size()));
    std::uint32_t offset = data_start;
    for (const OutItem& item : items_) {
      w.out().u16be(static_cast<std::uint16_t>(item.id)).u16be(0).u16be(1);
      w.out().u32be(offset).u32be(static_cast<std::uint32_t>(item.data.size()));
      offset += static_cast<std::uint32_t>(item.data.size());
    }
    CVT_TRY(w.end(iloc));

    const std::size_t iinf = w.begin_full("iinf", 0, 0);
    w.out().u16be(static_cast<std::uint16_t>(items_.size()));
    for (const OutItem& item : items_) {
      const std::size_t infe = w.begin_full("infe", 2, item.hidden ? 1 : 0);
      w.out().u16be(static_cast<std::uint16_t>(item.id)).u16be(0).text(item.type).u8(0);
      if (item.type == "mime") w.out().text(item.content_type).u8(0);
      CVT_TRY(w.end(infe));
    }
    CVT_TRY(w.end(iinf));

    if (!refs_.empty()) {
      const std::size_t iref = w.begin_full("iref", 0, 0);
      for (const OutRef& ref : refs_) {
        const std::size_t box = w.begin(ref.type);
        w.out().u16be(static_cast<std::uint16_t>(ref.from)).u16be(static_cast<std::uint16_t>(ref.to.size()));
        for (const std::uint32_t to : ref.to) w.out().u16be(static_cast<std::uint16_t>(to));
        CVT_TRY(w.end(box));
      }
      CVT_TRY(w.end(iref));
    }

    const std::size_t iprp = w.begin("iprp");
    const std::size_t ipco = w.begin("ipco");
    for (const Bytes& prop : properties_) w.out().bytes(prop);
    CVT_TRY(w.end(ipco));
    const bool wide_index = properties_.size() > 127;
    const std::size_t ipma = w.begin_full("ipma", 0, wide_index ? 1 : 0);
    std::uint32_t entries = 0;
    for (const OutItem& item : items_) entries += item.properties.empty() ? 0 : 1;
    w.out().u32be(entries);
    for (const OutItem& item : items_) {
      if (item.properties.empty()) continue;
      w.out().u16be(static_cast<std::uint16_t>(item.id)).u8(static_cast<std::uint8_t>(item.properties.size()));
      for (const auto& [index, essential] : item.properties) {
        if (wide_index) {
          w.out().u16be(static_cast<std::uint16_t>((essential ? 0x8000u : 0u) | index));
        } else {
          w.out().u8(static_cast<std::uint8_t>((essential ? 0x80u : 0u) | index));
        }
      }
    }
    CVT_TRY(w.end(ipma));
    CVT_TRY(w.end(iprp));
    CVT_TRY(w.end(meta));
    return w.take();
  }

  std::vector<OutItem> items_;
  std::vector<Bytes> properties_;
  std::vector<OutRef> refs_;
};

// ----------------------------------------------------------------------------- properties

Bytes box_bytes(std::string_view type, ByteView body) {
  BoxWriter w;
  const std::size_t mark = w.begin(type);
  w.out().bytes(body);
  (void)w.end(mark);
  return w.take();
}

Bytes ispe_box(std::uint32_t width, std::uint32_t height) {
  BoxWriter w;
  const std::size_t mark = w.begin_full("ispe", 0, 0);
  w.out().u32be(width).u32be(height);
  (void)w.end(mark);
  return w.take();
}

Bytes pixi_box(int channels, int bits) {
  BoxWriter w;
  const std::size_t mark = w.begin_full("pixi", 0, 0);
  w.out().u8(static_cast<std::uint8_t>(channels));
  for (int i = 0; i < channels; ++i) w.out().u8(static_cast<std::uint8_t>(bits));
  (void)w.end(mark);
  return w.take();
}

Bytes nclx_box(const Cicp& cicp) {
  BoxWriter w;
  const std::size_t mark = w.begin("colr");
  w.out().text("nclx").u16be(cicp.primaries).u16be(cicp.transfer).u16be(cicp.matrix).u8(cicp.full_range ? 0x80 : 0);
  (void)w.end(mark);
  return w.take();
}

Bytes icc_box(ByteView icc) {
  BoxWriter w;
  const std::size_t mark = w.begin("colr");
  w.out().text("prof").bytes(icc);
  (void)w.end(mark);
  return w.take();
}

Bytes auxc_box(std::string_view urn) {
  BoxWriter w;
  const std::size_t mark = w.begin_full("auxC", 0, 0);
  w.out().text(urn).u8(0);
  (void)w.end(mark);
  return w.take();
}

/// Clean aperture that keeps the top-left `width` x `height` of a
/// `coded_width` x `coded_height` picture.
Bytes clap_box(std::uint32_t width, std::uint32_t height, std::uint32_t coded_width, std::uint32_t coded_height) {
  BoxWriter w;
  const std::size_t mark = w.begin("clap");
  const std::int32_t dx = static_cast<std::int32_t>(width) - static_cast<std::int32_t>(coded_width);
  const std::int32_t dy = static_cast<std::int32_t>(height) - static_cast<std::int32_t>(coded_height);
  w.out().u32be(width).u32be(1).u32be(height).u32be(1).i32be(dx).u32be(2).i32be(dy).u32be(2);
  (void)w.end(mark);
  return w.take();
}

// ---------------------------------------------------------------------------- pixel prep

codec::EncodePreset resolved_effort(const Options& options) {
  const std::string effort = options.get_enum("effort", "balanced", {"fast", "balanced", "best"});
  if (effort == "fast") return codec::EncodePreset::Fast;
  if (effort == "best") return codec::EncodePreset::Small;
  return codec::EncodePreset::Balanced;
}

ir::VideoFrame rgba_view(const ir::Frame& frame) {
  ir::VideoFrame out;
  out.width = frame.width;
  out.height = frame.height;
  out.format = ir::PixelFormat::RGBA8;
  out.planes.resize(1);
  out.planes[0].data = frame.rgba;
  out.planes[0].stride = std::size_t{frame.width} * 4;
  return out;
}

/// The picture as 16-bit RGBA: from the HDR pixels when they hold more
/// precision within the SDR range, else the 8-bit frame expanded.
std::vector<std::uint16_t> rgba16_of(const ir::Image& image) {
  const ir::Frame& frame = image.frames.front();
  if (image.hdr && image.hdr->width == frame.width && image.hdr->height == frame.height &&
      image.hdr->rgba.size() == frame.rgba.size()) {
    bool in_range = true;
    for (std::size_t i = 0; i < image.hdr->rgba.size() && in_range; ++i) {
      if (i % 4 != 3 && !(image.hdr->rgba[i] <= 1.0001f)) in_range = false;
    }
    if (in_range) return image::srgb16_from_linear(*image.hdr);
  }
  std::vector<std::uint16_t> out(frame.rgba.size());
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint16_t>(frame.rgba[i] * 257u);
  return out;
}

void put10(ir::Plane& plane, std::size_t x, std::size_t y, std::uint32_t value) {
  const std::size_t at = plane.stride * y + x * 2;
  plane.data[at] = static_cast<std::uint8_t>(value & 0xFFu);
  plane.data[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

std::uint32_t to10(double v) { return static_cast<std::uint32_t>(std::clamp(v * 1023.0 + 0.5, 0.0, 1023.0)); }

/// 16-bit RGB -> 10-bit full-range BT.601 Y'CbCr 4:4:4, or GBR for `identity`.
ir::VideoFrame yuv10_444(const std::vector<std::uint16_t>& rgba, std::uint32_t width, std::uint32_t height,
                         bool identity) {
  ir::VideoFrame out = ir::VideoFrame::allocate(ir::PixelFormat::I444P10, width, height);
  constexpr double kr = 0.299;
  constexpr double kb = 0.114;
  constexpr double kg = 1.0 - kr - kb;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t at = (std::size_t{y} * width + x) * 4;
      const double r = rgba[at] / 65535.0;
      const double g = rgba[at + 1] / 65535.0;
      const double b = rgba[at + 2] / 65535.0;
      if (identity) {
        put10(out.planes[0], x, y, to10(g));
        put10(out.planes[1], x, y, to10(b));
        put10(out.planes[2], x, y, to10(r));
        continue;
      }
      const double luma = kr * r + kg * g + kb * b;
      put10(out.planes[0], x, y, to10(luma));
      put10(out.planes[1], x, y, to10((b - luma) / (2.0 * (1.0 - kb)) + 512.0 / 1023.0));
      put10(out.planes[2], x, y, to10((r - luma) / (2.0 * (1.0 - kr)) + 512.0 / 1023.0));
    }
  }
  out.color = {ir::ColorMatrix::BT601, ir::ColorRange::Full, ir::ChromaSiting::Center};
  return out;
}

/// An alpha plane as a monochrome-style 4:2:0 picture (luma = alpha, neutral chroma).
ir::VideoFrame alpha_picture(const ir::Image& image, bool ten_bit, const std::vector<std::uint16_t>* rgba16) {
  const ir::Frame& frame = image.frames.front();
  const ir::PixelFormat format = ten_bit ? ir::PixelFormat::I420P10 : ir::PixelFormat::I420;
  ir::VideoFrame out = ir::VideoFrame::allocate(format, frame.width, frame.height);
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      const std::size_t at = (std::size_t{y} * frame.width + x) * 4 + 3;
      if (ten_bit) {
        const double a = rgba16 != nullptr ? (*rgba16)[at] / 65535.0 : frame.rgba[at] / 255.0;
        put10(out.planes[0], x, y, to10(a));
      } else {
        out.planes[0].data[out.planes[0].stride * y + x] = frame.rgba[at];
      }
    }
  }
  for (std::size_t p = 1; p < 3; ++p) {
    if (ten_bit) {
      for (std::size_t i = 0; i + 1 < out.planes[p].data.size(); i += 2) {
        out.planes[p].data[i] = 0x00;
        out.planes[p].data[i + 1] = 0x02;  // 512
      }
    } else {
      std::fill(out.planes[p].data.begin(), out.planes[p].data.end(), std::uint8_t{128});
    }
  }
  out.color = {ir::ColorMatrix::BT601, ir::ColorRange::Full, ir::ChromaSiting::Center};
  return out;
}

// ----------------------------------------------------------------------------------- AVIF

struct AvifSettings {
  int quality = 90;
  bool lossless = false;
  bool ten_bit = false;
  bool chroma444 = false;
  codec::EncodePreset preset = codec::EncodePreset::Balanced;
};

Result<void> add_avif_items(FileBuilder& file, const ir::Image& image, const AvifSettings& s, std::uint32_t& color_id,
                            const WriteContext& ctx) {
  const ir::Frame& frame = image.frames.front();
  const bool alpha = !image::is_opaque(frame);
  Cicp cicp{1, 13, static_cast<std::uint16_t>(s.lossless ? 0 : 6), true};

  av1::StillEncodeOptions options;
  options.quality = s.quality;
  options.lossless = s.lossless;
  options.preset = s.preset;
  options.color = {static_cast<std::uint8_t>(cicp.primaries), static_cast<std::uint8_t>(cicp.transfer),
                   static_cast<std::uint8_t>(cicp.matrix), true, 0};

  std::vector<std::uint16_t> rgba16;
  ir::VideoFrame picture;
  if (s.ten_bit) {
    rgba16 = rgba16_of(image);
    picture = yuv10_444(rgba16, frame.width, frame.height, s.lossless);
    if (!s.lossless && !s.chroma444) {
      CVT_TRY_ASSIGN(picture, codec::convert_frame(picture, ir::PixelFormat::I420P10, picture.color));
    }
  } else if (s.lossless) {
    picture = rgba_view(frame);  // packed as G, B, R planes by the encoder
  } else {
    const ir::ColorInfo color{ir::ColorMatrix::BT601, ir::ColorRange::Full, ir::ChromaSiting::Center};
    CVT_TRY_ASSIGN(picture, codec::from_rgba(rgba_view(frame), s.chroma444 ? ir::PixelFormat::I444 : ir::PixelFormat::I420,
                                             color));
  }
  CVT_TRY(ctx.checkpoint(0.1, "Encoding"));
  CVT_TRY_ASSIGN(av1::EncodedStill still, av1::encode_still(picture, options));
  color_id = file.add_item("av01", std::move(still.obus));
  file.link(color_id, box_bytes("av1C", still.av1c), true);
  file.link(color_id, ispe_box(frame.width, frame.height), false);
  file.link(color_id, pixi_box(3, s.ten_bit ? 10 : 8), false);
  file.link(color_id, nclx_box(cicp), false);

  if (alpha) {
    CVT_TRY(ctx.checkpoint(0.6, "Encoding transparency"));
    av1::StillEncodeOptions alpha_options;
    alpha_options.quality = s.quality;
    alpha_options.lossless = s.lossless;
    alpha_options.preset = s.preset;
    alpha_options.monochrome = true;
    alpha_options.color.full_range = true;
    const ir::VideoFrame mask = alpha_picture(image, s.ten_bit, s.ten_bit ? &rgba16 : nullptr);
    CVT_TRY_ASSIGN(av1::EncodedStill encoded, av1::encode_still(mask, alpha_options));
    const std::uint32_t alpha_id = file.add_item("av01", std::move(encoded.obus));
    file.link(alpha_id, box_bytes("av1C", encoded.av1c), true);
    file.link(alpha_id, ispe_box(frame.width, frame.height), false);
    file.link(alpha_id, pixi_box(1, s.ten_bit ? 10 : 8), false);
    file.link(alpha_id, auxc_box("urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"), false);
    file.reference("auxl", alpha_id, {color_id});
  }
  return {};
}

// ----------------------------------------------------------------------------------- HEIC

/// The colour signalling of an encoded HEVC picture's VUI, so that the
/// 'colr' box agrees with the bitstream.
std::optional<Cicp> vui_colour(ByteView hvcc) {
  const auto config = hevc::parse_hvcc(hvcc);
  if (!config) return std::nullopt;
  for (const Bytes& nal : config->nals) {
    if (hevc::nal_type(nal) != static_cast<std::uint8_t>(hevc::NalType::Sps)) continue;
    const auto sps = hevc::parse_sps(nal);
    if (!sps || !sps->vui_present) return std::nullopt;
    return Cicp{sps->colour_primaries, sps->transfer_characteristics, sps->matrix_coefficients, sps->video_full_range};
  }
  return std::nullopt;
}

/// Encode one HEVC item (a single picture, or a grid of 512x512 tiles when
/// the picture exceeds what one HEVC picture may hold). `vui` receives the
/// colour signalling of the bitstream.
Result<std::uint32_t> add_hevc_image(FileBuilder& file, const ir::VideoFrame& picture,
                                     const hevc::PictureEncodeOptions& options, bool alpha_item,
                                     const WriteContext& ctx, double progress_from, double progress_to,
                                     std::optional<Cicp>* vui = nullptr) {
  const std::uint32_t width = picture.width;
  const std::uint32_t height = picture.height;
  const bool single = width <= hevc::max_dimension && height <= hevc::max_dimension &&
                      std::uint64_t{width} * height <= hevc::max_luma_samples;
  const auto describe = [&](std::uint32_t id, std::uint32_t coded_w, std::uint32_t coded_h, std::uint32_t w,
                            std::uint32_t h, bool with_clap) {
    file.link(id, ispe_box(coded_w, coded_h), false);
    file.link(id, pixi_box(3, 8), false);
    if (with_clap && (coded_w != w || coded_h != h)) file.link(id, clap_box(w, h, coded_w, coded_h), true);
  };
  if (single) {
    CVT_TRY_ASSIGN(hevc::EncodedPicture coded, hevc::encode_picture(picture, options));
    if (vui != nullptr) *vui = vui_colour(coded.hvcc);
    const std::uint32_t id = file.add_item("hvc1", std::move(coded.sample));
    file.link(id, box_bytes("hvcC", coded.hvcc), true, alpha_item);
    describe(id, coded.width, coded.height, width, height, true);
    return id;
  }

  // A grid of equal tiles; the right and bottom tiles repeat the edge pixels.
  const std::uint32_t columns = (width + hevc_tile_size - 1) / hevc_tile_size;
  const std::uint32_t rows = (height + hevc_tile_size - 1) / hevc_tile_size;
  if (columns > 256 || rows > 256 || columns * rows > 16384) {
    return fail(Status::Encode, "the picture is too large for a HEIC grid");
  }
  std::vector<std::uint32_t> tiles;
  const ir::PixelFormat format = picture.format;
  for (std::uint32_t row = 0; row < rows; ++row) {
    for (std::uint32_t column = 0; column < columns; ++column) {
      const double done = static_cast<double>(row * columns + column) / (rows * columns);
      CVT_TRY(ctx.checkpoint(progress_from + (progress_to - progress_from) * done, "Encoding tiles"));
      ir::VideoFrame tile = ir::VideoFrame::allocate(format, hevc_tile_size, hevc_tile_size);
      const std::size_t bytes_per_pixel = format == ir::PixelFormat::RGBA8 ? 4 : 1;
      for (std::size_t p = 0; p < tile.planes.size(); ++p) {
        const std::uint32_t sx = (p == 0 || format == ir::PixelFormat::RGBA8) ? 0 : 1;
        const std::uint32_t tw = ir::plane_width(format, p, hevc_tile_size);
        const std::uint32_t th = ir::plane_height(format, p, hevc_tile_size);
        const std::uint32_t pw = ir::plane_width(format, p, width);
        const std::uint32_t ph = ir::plane_height(format, p, height);
        const std::uint32_t x0 = (column * hevc_tile_size) >> sx;
        const std::uint32_t y0 = (row * hevc_tile_size) >> sx;
        for (std::uint32_t y = 0; y < th; ++y) {
          const std::uint32_t srcy = std::min(y0 + y, ph - 1);
          for (std::uint32_t x = 0; x < tw; ++x) {
            const std::uint32_t srcx = std::min(x0 + x, pw - 1);
            for (std::size_t b = 0; b < bytes_per_pixel; ++b) {
              tile.planes[p].data[tile.planes[p].stride * y + x * bytes_per_pixel + b] =
                  picture.planes[p].data[picture.planes[p].stride * srcy + srcx * bytes_per_pixel + b];
            }
          }
        }
      }
      tile.color = picture.color;
      CVT_TRY_ASSIGN(hevc::EncodedPicture coded, hevc::encode_picture(tile, options));
      if (vui != nullptr && tiles.empty()) *vui = vui_colour(coded.hvcc);
      const std::uint32_t id = file.add_item("hvc1", std::move(coded.sample), true);
      file.link(id, box_bytes("hvcC", coded.hvcc), true);
      describe(id, coded.width, coded.height, hevc_tile_size, hevc_tile_size, true);
      tiles.push_back(id);
    }
  }
  util::ByteWriter grid;
  grid.u8(0).u8(1).u8(static_cast<std::uint8_t>(rows - 1)).u8(static_cast<std::uint8_t>(columns - 1));
  grid.u32be(width).u32be(height);
  const std::uint32_t grid_id = file.add_item("grid", grid.take());
  file.link(grid_id, ispe_box(width, height), false);
  file.link(grid_id, pixi_box(alpha_item ? 1 : 3, 8), false);
  file.reference("dimg", grid_id, tiles);
  return grid_id;
}

Result<void> add_heic_items(FileBuilder& file, const ir::Image& image, int quality, codec::EncodePreset preset,
                            std::uint32_t& color_id, const WriteContext& ctx) {
  // The HEVC encoder codes 4:2:0 only, so an alpha item is a 4:2:0 picture
  // (luma = alpha). libheif reads that; Apple's decoder refuses the whole
  // file, so transparency is kept only on request.
  ir::Frame flattened;
  const ir::Frame* source = &image.frames.front();
  const bool keep_alpha = ctx.options.get_bool("keep_alpha", false);
  if (!keep_alpha && !image::is_opaque(*source)) {
    CVT_TRY_ASSIGN(flattened, image::opaque_frame(*source, ctx, "HEIC (without keep_alpha)"));
    source = &flattened;
  }
  const ir::Frame& frame = *source;
  const bool alpha = !image::is_opaque(frame);
  const Cicp cicp{1, 13, 6, true};
  hevc::PictureEncodeOptions options;
  options.quality = quality;
  options.preset = preset;
  options.color = {ir::ColorMatrix::BT601, ir::ColorRange::Full, ir::ChromaSiting::Center};
  CVT_TRY_ASSIGN(const ir::VideoFrame yuv, codec::from_rgba(rgba_view(frame), ir::PixelFormat::I420, options.color));
  std::optional<Cicp> vui;
  CVT_TRY_ASSIGN(color_id, add_hevc_image(file, yuv, options, false, ctx, 0.05, alpha ? 0.6 : 0.95, &vui));
  // Readers must see one description: the bitstream's when it has one.
  file.link(color_id, nclx_box(vui.value_or(cicp)), false);
  if (alpha) {
    const ir::VideoFrame mask = alpha_picture(image, false, nullptr);
    CVT_TRY_ASSIGN(const std::uint32_t alpha_id, add_hevc_image(file, mask, options, true, ctx, 0.6, 0.95));
    file.link(alpha_id, auxc_box("urn:mpeg:hevc:2015:auxid:1"), false);
    file.reference("auxl", alpha_id, {color_id});
  }
  return {};
}

// ------------------------------------------------------------------------------- metadata

void add_metadata(FileBuilder& file, const ir::Image& image, std::uint32_t color_id) {
  if (!image.icc_profile.empty()) file.link(color_id, icc_box(image.icc_profile), false);
  if (!image.exif.empty()) {
    ByteView tiff = image.exif;
    if (tiff.size() >= 6 && as_chars(tiff.first(6)) == image::exif_app1_prefix) tiff = tiff.subspan(6);
    Bytes exif = to_bytes(tiff);
    // The pixels are upright (the IR guarantees it), so the orientation must say so.
    image::reset_exif_orientation(exif);
    util::ByteWriter data(exif.size() + 4);
    data.u32be(0).bytes(exif);
    const std::uint32_t id = file.add_item("Exif", data.take());
    file.reference("cdsc", id, {color_id});
  }
  if (!image.xmp.empty()) {
    const std::uint32_t id = file.add_item("mime", to_bytes(std::string_view(image.xmp)));
    file.item(id).content_type = "application/rdf+xml";
    file.reference("cdsc", id, {color_id});
  }
}

}  // namespace

Result<Bytes> write_heif(const ir::Image& source, WriteContext& ctx, Flavor flavor) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  const std::string_view name = flavor == Flavor::Avif ? "AVIF" : flavor == Flavor::Heic ? "HEIC" : "HEIF";
  image::warn_if_animated(*prepared, ctx, name);
  const ir::Image& image = *prepared;
  const codec::EncodePreset preset = resolved_effort(ctx.options);
  const int quality = options::resolved_quality(ctx.options);

  FileBuilder file;
  std::uint32_t color_id = 0;
  if (flavor == Flavor::Avif) {
    AvifSettings s;
    s.quality = quality;
    s.lossless = options::resolved_lossless(ctx.options);
    s.preset = preset;
    const std::string depth = ctx.options.get_enum("bit_depth", "auto", {"auto", "8", "10"});
    s.ten_bit = depth == "10" || (depth == "auto" && image.source_bit_depth > 8);
    s.chroma444 = s.lossless || ctx.options.get_enum("chroma", "420", {"420", "444"}) == "444";
    CVT_TRY(add_avif_items(file, image, s, color_id, ctx));
  } else {
    CVT_TRY(add_heic_items(file, image, quality, preset, color_id, ctx));
  }
  if (prepared.keep_metadata()) add_metadata(file, image, color_id);
  CVT_TRY(ctx.checkpoint(0.97, "Writing"));

  switch (flavor) {
    case Flavor::Avif: return file.build("avif", {"avif", "mif1", "miaf"}, color_id);
    case Flavor::Heic: return file.build("heic", {"mif1", "heic", "miaf"}, color_id);
    case Flavor::Heif: return file.build("mif1", {"mif1", "heic", "miaf"}, color_id);
  }
  return fail(Status::Internal, "unknown HEIF flavour");
}

}  // namespace cvt::image_modern::heif
