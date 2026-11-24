// Convertible core - image family: Adobe Photoshop documents (PSD, PSB).
//
// Reader: the flattened composite ("merged image data") of bitmap,
// greyscale, indexed, RGB, CMYK, duotone and multichannel documents at 1, 8,
// 16 and 32 bits, raw or RLE (PackBits) compressed; the composite's alpha is
// un-blended from white as Photoshop stores it; resolution, ICC profile and
// XMP from the image resources. Layers are not composited (the merged image
// is what every application shows).
// Writer: a flat 8-bit RGB / greyscale document (RGBA with alpha), RLE.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

bool unpack_row(util::ByteReader& r, std::size_t length, std::uint8_t* out, std::size_t count) {
  util::ByteReader row = r.sub(length);
  if (!r.ok()) return false;
  std::size_t o = 0;
  while (o < count && !row.at_end()) {
    const auto n = static_cast<std::int8_t>(row.u8());
    if (n >= 0) {
      for (int k = 0; k <= n && o < count; ++k) out[o++] = row.u8();
    } else if (n != -128) {
      const std::uint8_t v = row.u8();
      for (int k = 0; k <= -n && o < count; ++k) out[o++] = v;
    }
  }
  return row.ok() && o == count;
}

Result<ir::Image> read_psd(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("8BPS")) return fail(Status::Decode, "not a Photoshop document");
  const std::uint16_t version = r.u16be();
  r.skip(6);
  const std::uint16_t channels = r.u16be();
  const std::uint32_t height = r.u32be();
  const std::uint32_t width = r.u32be();
  const std::uint16_t depth = r.u16be();
  const std::uint16_t mode = r.u16be();
  if (!r.ok()) return fail(Status::Decode, "Photoshop header is truncated");
  if (version != 1 && version != 2) return fail(Status::Decode, "Photoshop document version {} is not supported", version);
  const bool big = version == 2;
  if (channels == 0 || channels > 56) return fail(Status::Decode, "Photoshop channel count is invalid");
  if (depth != 1 && depth != 8 && depth != 16 && depth != 32) return fail(Status::Decode, "Photoshop depth {} is not supported", depth);
  if (mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 7 && mode != 8) {
    return fail(Status::Decode, "Photoshop colour mode {} is not supported", mode);
  }
  CVT_TRY(check_size(ctx, width, height));
  // Colour mode data (palette for indexed images).
  const std::uint32_t mode_length = r.u32be();
  const ByteView mode_data = r.bytes(mode_length);
  // Image resources.
  const std::uint32_t resources_length = r.u32be();
  util::ByteReader res = r.sub(resources_length);
  // Layer and mask information: skipped (the composite follows).
  const std::uint64_t layers_length = big ? r.u64be() : r.u32be();
  if (!r.ok() || layers_length > r.remaining()) return fail(Status::Decode, "Photoshop document is truncated");
  r.skip(static_cast<std::size_t>(layers_length));
  const std::uint16_t compression = r.u16be();
  if (!r.ok()) return fail(Status::Decode, "Photoshop document has no composite image");
  if (compression > 1) return fail(Status::Decode, "Photoshop composite compression {} is not supported", compression);

  ir::Image image;
  // Resources: resolution (1005), ICC (1039), XMP (1060).
  while (res.remaining() >= 12) {
    if (!res.consume("8BIM")) break;
    const std::uint16_t id = res.u16be();
    const std::uint8_t name_length = res.u8();
    res.skip(name_length + ((name_length + 1) % 2));
    const std::uint32_t size = res.u32be();
    const ByteView data = res.bytes(size);
    if (size % 2) res.skip(1);
    if (!res.ok()) break;
    if (id == 1005 && data.size() >= 16) {
      // ResolutionInfo: 16.16 fixed-point horizontal and vertical pixels per inch.
      util::ByteReader d(data);
      const double hres = d.u32be() / 65536.0;
      d.skip(4);
      const double vres = d.u32be() / 65536.0;
      if (hres > 0 && vres > 0) {
        image.dpi_x = hres;
        image.dpi_y = vres;
      }
    } else if (id == 1039) {
      image.icc_profile = to_bytes(data);
    } else if (id == 1060) {
      image.xmp = util::sanitize_utf8(as_chars(data));
    }
  }

  const std::uint64_t row_bytes = depth == 1 ? (std::uint64_t{width} + 7) / 8 : std::uint64_t{width} * (depth / 8);
  const std::uint64_t plane_bytes = row_bytes * height;
  CVT_TRY(check_plausible(std::uint64_t{width} * height, r.remaining(), compression == 1 ? 128 : 4));
  // Only the channels we interpret are decoded.
  const std::uint32_t colour = mode == 4 ? 4u : (mode == 3 ? 3u : 1u);
  const bool alpha = channels > colour && mode != 7 && mode != 0;
  const std::uint32_t used = std::min<std::uint32_t>(channels, colour + (alpha ? 1 : 0));
  CVT_TRY(ctx.limits.check_decompressed(plane_bytes * used));
  std::vector<std::vector<std::uint8_t>> planes(used, std::vector<std::uint8_t>(static_cast<std::size_t>(plane_bytes), 0));
  bool truncated = false;
  if (compression == 0) {
    for (std::uint32_t c = 0; c < used; ++c) {
      const ByteView data = r.bytes(std::min<std::size_t>(static_cast<std::size_t>(plane_bytes), r.remaining()));
      std::memcpy(planes[c].data(), data.data(), data.size());
      if (data.size() < plane_bytes) {
        truncated = true;
        break;
      }
    }
  } else {
    const std::uint64_t rows = std::uint64_t{height} * channels;
    std::vector<std::uint32_t> lengths(static_cast<std::size_t>(rows));
    for (auto& l : lengths) l = big ? r.u32be() : r.u16be();
    if (!r.ok()) return fail(Status::Decode, "Photoshop row table is truncated");
    for (std::uint32_t c = 0; c < used && !truncated; ++c) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(c) / used));
      for (std::uint32_t y = 0; y < height; ++y) {
        if (!unpack_row(r, lengths[std::size_t{c} * height + y], planes[c].data() + row_bytes * y, static_cast<std::size_t>(row_bytes))) {
          truncated = true;
          break;
        }
      }
    }
  }
  if (truncated) ctx.warn("The Photoshop document is truncated; missing parts were left black.");

  const auto sample = [&](std::uint32_t c, std::uint64_t index) -> double {
    const std::uint8_t* p = planes[c].data();
    switch (depth) {
      case 1: return ((p[index / 8] >> (7 - index % 8)) & 1u) ? 0.0 : 1.0;  // bitmap: 1 = black
      case 8: return p[index] / 255.0;
      case 16: return (p[index * 2] << 8 | p[index * 2 + 1]) / 65535.0;
      default: {
        const std::uint32_t bits = static_cast<std::uint32_t>(p[index * 4]) << 24 | static_cast<std::uint32_t>(p[index * 4 + 1]) << 16 |
                                   static_cast<std::uint32_t>(p[index * 4 + 2]) << 8 | p[index * 4 + 3];
        const float f = std::bit_cast<float>(bits);
        return std::isfinite(f) ? f : 0.0;
      }
    }
  };
  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  const bool wide = depth >= 16;
  std::optional<ir::HdrPixels> hdr;
  if (wide) {
    hdr.emplace();
    hdr->width = width;
    hdr->height = height;
    hdr->rgba.assign(std::size_t{width} * height * 4, 1.0f);
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint64_t i = std::uint64_t{y} * width + x;
      double rgb[3];
      double a = 1.0;
      if (mode == 2 && depth == 8) {
        const std::uint8_t index = planes[0][static_cast<std::size_t>(i)];
        for (int c = 0; c < 3; ++c) {
          const std::size_t at = static_cast<std::size_t>(c) * 256 + index;
          rgb[c] = at < mode_data.size() ? std::to_integer<std::uint8_t>(mode_data[at]) / 255.0 : 0.0;
        }
      } else if (mode == 3) {
        for (std::uint32_t c = 0; c < 3; ++c) rgb[c] = sample(c, i);
      } else if (mode == 4) {
        // Photoshop stores CMYK inverted (0 = full ink).
        const double k = sample(3, i);
        for (std::uint32_t c = 0; c < 3; ++c) rgb[c] = sample(c, i) * k;
      } else {
        rgb[0] = rgb[1] = rgb[2] = sample(0, i);
      }
      if (alpha) {
        a = std::clamp(sample(colour, i), 0.0, 1.0);
        // The composite is blended onto white: undo it.
        if (a > 0.0) {
          for (double& v : rgb) v = (v - (1.0 - a)) / a;
        }
      }
      std::uint8_t* d = frame.rgba.data() + frame.offset(x, y);
      for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>(std::clamp(rgb[c] * 255.0 + 0.5, 0.0, 255.0));
      d[3] = static_cast<std::uint8_t>(std::clamp(a * 255.0 + 0.5, 0.0, 255.0));
      if (hdr) {
        float* q = hdr->rgba.data() + i * 4;
        for (int c = 0; c < 3; ++c) {
          const double v = std::max(rgb[c], 0.0);
          q[c] = depth == 32 ? static_cast<float>(v) : static_cast<float>(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
        }
        q[3] = static_cast<float>(a);
      }
    }
  }
  if (depth == 32 && hdr) frame = tone_map(*hdr);
  image.frames.push_back(std::move(frame));
  image.hdr = std::move(hdr);
  image.source_bit_depth = static_cast<std::uint8_t>(depth);
  image.source_grayscale = mode == 1 || mode == 0 || mode == 8;
  return image;
}

Result<Bytes> write_psd(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "PSD");
  const ir::Frame& frame = prepared->frames.front();
  const bool big = ctx.format_id == "psb";
  if (!big && (frame.width > 30000 || frame.height > 30000)) {
    return fail(Status::Encode, "PSD documents are limited to 30000x30000 pixels; use PSB");
  }
  const bool alpha = !is_opaque(frame);
  const bool grey = is_grayscale(frame);
  const std::uint16_t colour = grey ? 1 : 3;
  const std::uint16_t channels = static_cast<std::uint16_t>(colour + (alpha ? 1 : 0));
  util::ByteWriter out;
  out.text("8BPS").u16be(big ? 2 : 1).zeros(6).u16be(channels).u32be(frame.height).u32be(frame.width).u16be(8);
  out.u16be(grey ? 1 : 3);
  out.u32be(0);  // colour mode data
  // Image resources: resolution, and metadata when kept.
  util::ByteWriter res;
  const auto resource = [&](std::uint16_t id, ByteView data) {
    res.text("8BIM").u16be(id).u16be(0).u32be(static_cast<std::uint32_t>(data.size())).bytes(data);
    if (data.size() % 2) res.u8(0);
  };
  {
    const double dx = prepared->dpi_x > 0 ? prepared->dpi_x : 72.0;
    const double dy = prepared->dpi_y > 0 ? prepared->dpi_y : 72.0;
    util::ByteWriter info;
    info.u32be(static_cast<std::uint32_t>(std::lround(dx * 65536.0))).u16be(1).u16be(1);
    info.u32be(static_cast<std::uint32_t>(std::lround(dy * 65536.0))).u16be(1).u16be(1);
    resource(1005, info.view());
  }
  if (prepared.keep_metadata()) {
    if (!prepared->icc_profile.empty()) resource(1039, prepared->icc_profile);
    if (!prepared->xmp.empty()) resource(1060, as_bytes(prepared->xmp));
  }
  out.u32be(static_cast<std::uint32_t>(res.size())).bytes(res.view());
  if (big) {
    out.u64be(0);
  } else {
    out.u32be(0);  // no layers
  }
  out.u16be(1);  // RLE
  const std::uint32_t w = frame.width;
  const std::uint32_t h = frame.height;
  std::vector<std::vector<std::uint8_t>> rows;
  rows.reserve(std::size_t{h} * channels);
  std::vector<std::uint8_t> plain(w);
  for (std::uint16_t c = 0; c < channels; ++c) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(c) / channels));
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
        if (c < colour) {
          // Blend onto white, as Photoshop stores the composite.
          const std::uint32_t a = p[3];
          plain[x] = static_cast<std::uint8_t>((p[grey ? 0 : c] * a + 255u * (255u - a) + 127u) / 255u);
        } else {
          plain[x] = p[3];
        }
      }
      std::vector<std::uint8_t> packed;
      std::size_t i = 0;
      while (i < w) {
        std::size_t run = 1;
        while (i + run < w && run < 128 && plain[i + run] == plain[i]) ++run;
        if (run >= 2) {
          packed.push_back(static_cast<std::uint8_t>(257 - run));
          packed.push_back(plain[i]);
          i += run;
          continue;
        }
        std::size_t n = 1;
        while (i + n < w && n < 128 && !(i + n + 1 < w && plain[i + n] == plain[i + n + 1])) ++n;
        packed.push_back(static_cast<std::uint8_t>(n - 1));
        packed.insert(packed.end(), plain.begin() + static_cast<std::ptrdiff_t>(i), plain.begin() + static_cast<std::ptrdiff_t>(i + n));
        i += n;
      }
      rows.push_back(std::move(packed));
    }
  }
  for (const auto& row : rows) {
    if (big) {
      out.u32be(static_cast<std::uint32_t>(row.size()));
    } else {
      out.u16be(static_cast<std::uint16_t>(row.size()));
    }
  }
  for (const auto& row : rows) out.bytes(std::span<const std::uint8_t>(row));
  return out.take();
}

}  // namespace

void register_psd(Registry& registry) {
  FormatInfo psd;
  psd.id = "psd";
  psd.name = "Adobe Photoshop document";
  psd.description = "Photoshop image (the flattened picture is read).";
  psd.category = Category::Image;
  psd.extensions = {"psd"};
  psd.mime = "image/vnd.adobe.photoshop";
  psd.popularity = 55;
  psd.signatures = {Signature::at(0, "'8BPS' 00 01")};
  registry.add_format(std::move(psd));

  FormatInfo psb;
  psb.id = "psb";
  psb.name = "Adobe Photoshop large document";
  psb.description = "Photoshop big document (PSB).";
  psb.category = Category::Image;
  psb.extensions = {"psb"};
  psb.mime = "image/vnd.adobe.photoshop";
  psb.popularity = 15;
  psb.signatures = {Signature::at(0, "'8BPS' 00 02")};
  registry.add_format(std::move(psb));

  for (const char* id : {"psd", "psb"}) {
    ImageCodecSpec spec;
    spec.format_id = id;
    spec.read = reader(read_psd);
    spec.write = writer(write_psd);
    spec.write_options = plain_writer_options();
    add_image_codec(registry, std::move(spec));
  }
}

}  // namespace cvt::image
