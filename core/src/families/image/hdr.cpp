// Convertible core - image family: Radiance RGBE (.hdr, .pic).
//
// Reader: "#?RADIANCE" / "#?RGBE" headers, 32-bit_rle_rgbe and _xyze
// formats, EXPOSURE, flat, old-style RLE and adaptive ("new") RLE
// scanlines, any of the eight orientation strings. The float pixels go to
// `ir::Image::hdr`; the 8-bit frame is tone mapped (options `tone_map`,
// `exposure`). Writer: adaptive RLE RGBE from the HDR pixels (or from the
// 8-bit picture, linearised).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
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

inline void rgbe_to_float(const std::uint8_t* e, float* out) {
  if (e[3] == 0) {
    out[0] = out[1] = out[2] = 0.0f;
    return;
  }
  const float f = std::ldexp(1.0f, static_cast<int>(e[3]) - (128 + 8));
  out[0] = (e[0] + 0.5f) * f;
  out[1] = (e[1] + 0.5f) * f;
  out[2] = (e[2] + 0.5f) * f;
}

inline void float_to_rgbe(const float* in, std::uint8_t* e) {
  const float r = std::isfinite(in[0]) ? std::max(in[0], 0.0f) : 0.0f;
  const float g = std::isfinite(in[1]) ? std::max(in[1], 0.0f) : 0.0f;
  const float b = std::isfinite(in[2]) ? std::max(in[2], 0.0f) : 0.0f;
  const float v = std::max({r, g, b});
  if (v < 1e-32f) {
    e[0] = e[1] = e[2] = e[3] = 0;
    return;
  }
  int exponent = 0;
  const float mantissa = std::frexp(v, &exponent);
  if (exponent > 127) {
    e[0] = e[1] = e[2] = 255;
    e[3] = 255;
    return;
  }
  const float scale = mantissa * 256.0f / v;
  e[0] = static_cast<std::uint8_t>(std::min(255.0f, r * scale));
  e[1] = static_cast<std::uint8_t>(std::min(255.0f, g * scale));
  e[2] = static_cast<std::uint8_t>(std::min(255.0f, b * scale));
  e[3] = static_cast<std::uint8_t>(exponent + 128);
}

/// Read one scanline of `width` RGBE pixels in any of the three encodings.
bool read_scanline(util::ByteReader& r, std::uint32_t width, std::uint8_t* out) {
  if (width >= 8 && width < 32768 && r.remaining() >= 4) {
    const ByteView head = r.peek_rest().first(4);
    const auto b = [&](std::size_t i) { return std::to_integer<std::uint32_t>(head[i]); };
    if (b(0) == 2 && b(1) == 2 && (b(2) & 0x80) == 0) {
      if ((b(2) << 8 | b(3)) != width) return false;
      r.skip(4);
      for (std::size_t c = 0; c < 4; ++c) {
        std::uint32_t x = 0;
        while (x < width) {
          std::uint32_t count = r.u8();
          if (!r.ok()) return false;
          if (count > 128) {
            count -= 128;
            const std::uint8_t value = r.u8();
            if (count > width - x || !r.ok()) return false;
            for (std::uint32_t k = 0; k < count; ++k) out[(x++) * 4 + c] = value;
          } else {
            if (count == 0 || count > width - x) return false;
            const ByteView values = r.bytes(count);
            if (!r.ok()) return false;
            for (std::uint32_t k = 0; k < count; ++k) out[(x++) * 4 + c] = std::to_integer<std::uint8_t>(values[k]);
          }
        }
      }
      return true;
    }
  }
  // Flat or old-style RLE: (1,1,1,n) repeats the previous pixel n << shift times.
  std::uint32_t x = 0;
  int shift = 0;
  while (x < width) {
    const ByteView p = r.bytes(4);
    if (!r.ok()) return false;
    const auto* q = as_u8(p).data();
    if (q[0] == 1 && q[1] == 1 && q[2] == 1) {
      if (x == 0) return false;
      const std::uint64_t count = std::uint64_t{q[3]} << shift;
      if (count > width - x) return false;
      for (std::uint64_t k = 0; k < count; ++k, ++x) std::memcpy(out + std::size_t{x} * 4, out + std::size_t{x - 1} * 4, 4);
      shift += 8;
      if (shift > 24) return false;
    } else {
      std::memcpy(out + std::size_t{x} * 4, q, 4);
      ++x;
      shift = 0;
    }
  }
  return true;
}

Result<ir::Image> read_hdr(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  if (!text.starts_with("#?RADIANCE") && !text.starts_with("#?RGBE")) return fail(Status::Decode, "not a Radiance HDR file");
  std::size_t pos = 0;
  bool xyz = false;
  bool rgbe = false;
  double exposure = 1.0;
  // Header lines until an empty line.
  while (true) {
    const std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) return fail(Status::Decode, "Radiance header is truncated");
    const std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    if (line.empty()) break;
    if (line.starts_with("FORMAT=")) {
      xyz = line.find("xyze") != std::string_view::npos;
      rgbe = !xyz;
    } else if (line.starts_with("EXPOSURE=")) {
      const auto v = util::parse_double(util::trim(line.substr(9)));
      if (v && *v > 0 && std::isfinite(*v)) exposure *= *v;
    }
    if (pos > 64 * 1024) return fail(Status::Decode, "Radiance header is too long");
  }
  (void)rgbe;
  const std::size_t end = text.find('\n', pos);
  if (end == std::string_view::npos) return fail(Status::Decode, "Radiance resolution line is missing");
  const std::vector<std::string_view> parts = util::split(text.substr(pos, end - pos), ' ', false);
  pos = end + 1;
  if (parts.size() != 4) return fail(Status::Decode, "Radiance resolution line is malformed");
  const auto n1 = util::parse_int(parts[1]);
  const auto n2 = util::parse_int(parts[3]);
  if (!n1 || !n2 || *n1 <= 0 || *n2 <= 0 || *n1 > 0x7FFFFFFF || *n2 > 0x7FFFFFFF) {
    return fail(Status::Decode, "Radiance resolution line is malformed");
  }
  // "-Y h +X w" is the standard; the axis named first is the slow one.
  const bool rows_first = parts[0].size() == 2 && parts[0][1] == 'Y';
  const bool flip_slow = parts[0].size() == 2 && parts[0][0] == '+';
  const bool flip_fast = parts[2].size() == 2 && parts[2][0] == '-';
  const auto stored_rows = static_cast<std::uint32_t>(*n1);
  const auto stored_cols = static_cast<std::uint32_t>(*n2);
  const std::uint32_t width = rows_first ? stored_cols : stored_rows;
  const std::uint32_t height = rows_first ? stored_rows : stored_cols;
  CVT_TRY(check_size(ctx, width, height));
  CVT_TRY(check_plausible(std::uint64_t{width} * height, ctx.input.size() - pos, 32));
  util::ByteReader r(ctx.input.subspan(pos));
  std::vector<std::uint8_t> line(std::size_t{stored_cols} * 4);
  ir::HdrPixels hdr;
  hdr.width = width;
  hdr.height = height;
  hdr.rgba.assign(std::size_t{width} * height * 4, 1.0f);
  bool truncated = false;
  for (std::uint32_t s = 0; s < stored_rows; ++s) {
    if ((s & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(s) / stored_rows));
    if (!read_scanline(r, stored_cols, line.data())) {
      truncated = true;
      break;
    }
    for (std::uint32_t f = 0; f < stored_cols; ++f) {
      const std::uint32_t slow = flip_slow ? stored_rows - 1 - s : s;
      const std::uint32_t fast = flip_fast ? stored_cols - 1 - f : f;
      const std::uint32_t x = rows_first ? fast : slow;
      const std::uint32_t y = rows_first ? slow : fast;
      float* p = hdr.rgba.data() + (std::size_t{y} * width + x) * 4;
      rgbe_to_float(line.data() + std::size_t{f} * 4, p);
      for (int c = 0; c < 3; ++c) p[c] = static_cast<float>(p[c] / exposure);
      if (xyz) {
        const float X = p[0], Y = p[1], Z = p[2];
        p[0] = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
        p[1] = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
        p[2] = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;
      }
    }
  }
  if (truncated) ctx.warn("The Radiance file is truncated or damaged; missing rows were left black.");
  ToneMapOptions tm;
  const std::string op = ctx.options.get_enum("tone_map", "auto", {"auto", "clamp", "reinhard", "filmic"});
  tm.op = op == "clamp" ? ToneMapOperator::Clamp : op == "reinhard" ? ToneMapOperator::Reinhard
          : op == "filmic" ? ToneMapOperator::Filmic : ToneMapOperator::Auto;
  tm.exposure_stops = static_cast<float>(ctx.options.get_float("exposure", 0.0, -16.0, 16.0));
  ir::Image image;
  image.frames.push_back(tone_map(hdr, tm));
  image.hdr = std::move(hdr);
  image.source_bit_depth = 32;
  return image;
}

Result<Bytes> write_hdr(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "Radiance HDR");
  const ir::Image& image = *prepared;
  const std::uint32_t w = image.width();
  const std::uint32_t h = image.height();
  const ir::HdrPixels pixels =
      image.hdr && image.hdr->width == w && image.hdr->height == h ? *image.hdr : to_linear(image.frames.front());
  util::ByteWriter out;
  out.text("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n");
  out.text(std::format("-Y {} +X {}\n", h, w));
  std::vector<std::uint8_t> line(std::size_t{w} * 4);
  std::vector<std::uint8_t> channel(w);
  for (std::uint32_t y = 0; y < h; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / h));
    for (std::uint32_t x = 0; x < w; ++x) float_to_rgbe(pixels.rgba.data() + (std::size_t{y} * w + x) * 4, line.data() + std::size_t{x} * 4);
    if (w < 8 || w > 32767) {
      out.bytes(std::span<const std::uint8_t>(line));
      continue;
    }
    out.u8(2).u8(2).u8(static_cast<std::uint8_t>(w >> 8)).u8(static_cast<std::uint8_t>(w));
    for (std::size_t c = 0; c < 4; ++c) {
      for (std::uint32_t x = 0; x < w; ++x) channel[x] = line[std::size_t{x} * 4 + c];
      std::uint32_t x = 0;
      while (x < w) {
        std::uint32_t run = 1;
        while (x + run < w && run < 127 && channel[x + run] == channel[x]) ++run;
        if (run >= 3) {
          out.u8(static_cast<std::uint8_t>(128 + run)).u8(channel[x]);
          x += run;
          continue;
        }
        std::uint32_t n = 0;
        while (x + n < w && n < 128) {
          if (x + n + 2 < w && channel[x + n] == channel[x + n + 1] && channel[x + n] == channel[x + n + 2]) break;
          ++n;
        }
        out.u8(static_cast<std::uint8_t>(n));
        out.bytes(std::span<const std::uint8_t>(channel).subspan(x, n));
        x += n;
      }
    }
  }
  return out.take();
}

}  // namespace

void register_hdr(Registry& registry) {
  FormatInfo info;
  info.id = "hdr";
  info.name = "Radiance HDR image";
  info.description = "High dynamic range RGBE image.";
  info.category = Category::Image;
  info.extensions = {"hdr", "pic", "rgbe"};
  info.mime = "image/vnd.radiance";
  info.popularity = 25;
  info.signatures = {Signature::at(0, "'#?RADIANCE'"), Signature::at(0, "'#?RGBE'")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "hdr";
  spec.read = reader(read_hdr);
  spec.write = writer(write_hdr);
  spec.write_options = plain_writer_options();
  spec.read_options = {
      OptionSpec::choice("tone_map", "Tone mapping", "auto", {"auto", "clamp", "reinhard", "filmic"},
                         "How HDR brightness is mapped to the normal range for formats that cannot store it.")
          .advanced(),
      OptionSpec::number("exposure", "Exposure", 0.0, -16.0, 16.0,
                         "Brightness adjustment in stops before tone mapping.")
          .advanced(),
  };
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
