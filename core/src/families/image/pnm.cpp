// Convertible core - image family: the Netpbm family and PFM.
//
// PBM, PGM, PPM (ASCII "P1"-"P3" and binary "P4"-"P6", maxval up to 65535),
// PAM ("P7" with BLACKANDWHITE, GRAYSCALE, RGB and their _ALPHA tuple types)
// and PFM (float RGB "PF" / grey "Pf", either byte order, bottom-up rows).
// 16-bit and float data keep their precision in `ir::Image::hdr`.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

class HeaderReader {
 public:
  explicit HeaderReader(ByteView data) : text_(as_chars(data)) {}
  void skip_space() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '#') {
        while (pos_ < text_.size() && text_[pos_] != '\n' && text_[pos_] != '\r') ++pos_;
      } else if (util::is_ascii_space(c)) {
        ++pos_;
      } else {
        break;
      }
    }
  }
  Result<std::uint32_t> number() {
    skip_space();
    std::uint64_t value = 0;
    std::size_t digits = 0;
    while (pos_ < text_.size() && util::is_ascii_digit(text_[pos_])) {
      value = value * 10 + static_cast<std::uint64_t>(text_[pos_++] - '0');
      if (++digits > 10 || value > 0xFFFFFFFFu) return fail(Status::Decode, "Netpbm header number is too large");
    }
    if (digits == 0) return fail(Status::Decode, "malformed Netpbm header");
    return static_cast<std::uint32_t>(value);
  }
  std::string_view token() {
    skip_space();
    const std::size_t start = pos_;
    while (pos_ < text_.size() && !util::is_ascii_space(text_[pos_])) ++pos_;
    return text_.substr(start, pos_ - start);
  }
  std::string_view line() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
    std::string_view l = text_.substr(start, pos_ - start);
    if (pos_ < text_.size()) ++pos_;
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    return l;
  }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  void advance(std::size_t n) noexcept { pos_ = std::min(pos_ + n, text_.size()); }
  [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
  [[nodiscard]] char peek() const noexcept { return pos_ < text_.size() ? text_[pos_] : '\0'; }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

enum class Tuple { BlackWhite, Grey, Rgb };

struct Layout {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t maxval = 255;
  std::uint32_t depth = 1;  // samples per pixel
  Tuple tuple = Tuple::Grey;
  bool alpha = false;
  bool ascii = false;
  bool packed_bits = false;  // P4 (1 = black)
  bool plain_bits = false;   // P1
};

Result<ir::Image> build(const Layout& l, HeaderReader& h, ByteView data, const Context& ctx) {
  CVT_TRY(check_size(ctx, l.width, l.height));
  if (l.maxval == 0 || l.maxval > 65535) return fail(Status::Decode, "Netpbm maxval {} is out of range", l.maxval);
  const bool wide = l.maxval > 255;
  const std::uint64_t samples = std::uint64_t{l.width} * l.height * l.depth;
  if (l.ascii || l.plain_bits) {
    CVT_TRY(check_plausible(std::uint64_t{l.width} * l.height, data.size(), 4));
  } else if (l.packed_bits) {
    CVT_TRY(check_plausible(std::uint64_t{l.width} * l.height, data.size(), 32));
  } else {
    CVT_TRY(check_plausible(samples * (wide ? 2 : 1), data.size(), 4));
  }
  std::vector<std::uint16_t> values(static_cast<std::size_t>(samples), 0);
  bool truncated = false;
  if (l.plain_bits) {
    for (std::uint64_t i = 0; i < samples; ++i) {
      h.skip_space();
      const char c = h.peek();
      if (c != '0' && c != '1') {
        truncated = true;
        break;
      }
      h.advance(1);
      values[i] = c == '1' ? 0 : 1;  // 1 = black
    }
  } else if (l.ascii) {
    for (std::uint64_t i = 0; i < samples; ++i) {
      if ((i & 0xFFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(samples)));
      auto v = h.number();
      if (!v) {
        truncated = true;
        break;
      }
      values[i] = static_cast<std::uint16_t>(std::min(*v, l.maxval));
    }
  } else {
    util::ByteReader r(data.subspan(std::min(h.position(), data.size())));
    if (l.packed_bits) {
      const std::size_t row = (l.width + 7) / 8;
      for (std::uint32_t y = 0; y < l.height && !truncated; ++y) {
        const ByteView bits = r.bytes(row);
        if (!r.ok()) {
          truncated = true;
          break;
        }
        for (std::uint32_t x = 0; x < l.width; ++x) {
          const bool black = (std::to_integer<std::uint8_t>(bits[x / 8]) >> (7 - x % 8)) & 1u;
          values[static_cast<std::size_t>(y) * l.width + x] = black ? 0 : 1;
        }
      }
    } else {
      for (std::uint64_t i = 0; i < samples; ++i) {
        values[i] = wide ? r.u16be() : r.u8();
        if (!r.ok()) {
          truncated = true;
          values[i] = 0;
          break;
        }
        if (values[i] > l.maxval) values[i] = static_cast<std::uint16_t>(l.maxval);
      }
    }
  }
  if (truncated) {
    if (samples > 0 && values.empty()) return fail(Status::Decode, "Netpbm pixel data is missing");
    ctx.warn("The Netpbm file is truncated; missing pixels were left black.");
  }
  const std::uint32_t maxval = l.packed_bits || l.plain_bits ? 1 : l.maxval;
  ir::Frame frame = ir::Frame::filled(l.width, l.height, {0, 0, 0, 255});
  std::vector<std::uint16_t> wide16;
  if (wide) wide16.resize(static_cast<std::size_t>(l.width) * l.height * 4);
  const std::uint32_t colour = l.tuple == Tuple::Rgb ? 3 : 1;
  for (std::size_t p = 0; p < static_cast<std::size_t>(l.width) * l.height; ++p) {
    const std::uint16_t* s = values.data() + p * l.depth;
    std::uint32_t rgb[3];
    for (std::uint32_t c = 0; c < 3; ++c) rgb[c] = s[colour == 3 ? c : 0];
    const std::uint32_t a = l.alpha && l.depth > colour ? s[colour] : maxval;
    for (std::uint32_t c = 0; c < 3; ++c) frame.rgba[p * 4 + c] = scale_max_to_8(rgb[c], maxval);
    frame.rgba[p * 4 + 3] = scale_max_to_8(a, maxval);
    if (wide) {
      for (std::uint32_t c = 0; c < 3; ++c) {
        wide16[p * 4 + c] = static_cast<std::uint16_t>((std::uint64_t{rgb[c]} * 65535 + maxval / 2) / maxval);
      }
      wide16[p * 4 + 3] = static_cast<std::uint16_t>((std::uint64_t{a} * 65535 + maxval / 2) / maxval);
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  if (wide) image.hdr = linear_from_srgb16(wide16, l.width, l.height);
  image.source_bit_depth = static_cast<std::uint8_t>(std::bit_width(maxval));
  image.source_grayscale = colour == 1;
  return image;
}

Result<ir::Image> read_pfm(ReadContext& ctx, HeaderReader& h, bool colour) {
  CVT_TRY_ASSIGN(const std::uint32_t width, h.number());
  CVT_TRY_ASSIGN(const std::uint32_t height, h.number());
  h.skip_space();
  const std::string scale_text(h.line());
  const auto scale = util::parse_double(util::trim(scale_text));
  if (!scale || *scale == 0.0 || !std::isfinite(*scale)) return fail(Status::Decode, "PFM scale is invalid");
  const bool little = *scale < 0;
  CVT_TRY(check_size(ctx, width, height));
  const std::uint32_t channels = colour ? 3 : 1;
  const std::uint64_t needed = std::uint64_t{width} * height * channels * 4;
  util::ByteReader r(ctx.input.subspan(std::min(h.position(), ctx.input.size())));
  if (r.remaining() < needed) return fail(Status::Decode, "PFM pixel data is truncated");
  ir::HdrPixels hdr;
  hdr.width = width;
  hdr.height = height;
  hdr.rgba.assign(static_cast<std::size_t>(width) * height * 4, 1.0f);
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::uint32_t y = height - 1 - row;  // bottom-up
    for (std::uint32_t x = 0; x < width; ++x) {
      float* p = hdr.rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      for (std::uint32_t c = 0; c < channels; ++c) {
        float v = little ? r.f32le() : r.f32be();
        if (!std::isfinite(v)) v = 0.0f;
        p[c] = v;
      }
      if (!colour) p[1] = p[2] = p[0];
    }
  }
  ir::Image image;
  ToneMapOptions tm;
  tm.exposure_stops = static_cast<float>(ctx.options.get_float("exposure", 0.0, -16.0, 16.0));
  image.frames.push_back(tone_map(hdr, tm));
  image.hdr = std::move(hdr);
  image.source_bit_depth = 32;
  image.source_grayscale = !colour;
  return image;
}

Result<ir::Image> read_pnm(ReadContext& ctx) {
  HeaderReader h(ctx.input);
  if (ctx.input.size() < 2 || std::to_integer<char>(ctx.input[0]) != 'P') return fail(Status::Decode, "not a Netpbm file");
  const char kind = std::to_integer<char>(ctx.input[1]);
  h.advance(2);
  if (kind == 'F' || kind == 'f') return read_pfm(ctx, h, kind == 'F');
  Layout l;
  if (kind == '7') {
    bool ended = false;
    std::string tuple;
    for (int guard = 0; guard < 64 && !h.at_end(); ++guard) {
      const std::string_view key = h.token();
      if (key == "ENDHDR") {
        h.line();
        ended = true;
        break;
      }
      if (key == "WIDTH") {
        CVT_TRY_ASSIGN(l.width, h.number());
      } else if (key == "HEIGHT") {
        CVT_TRY_ASSIGN(l.height, h.number());
      } else if (key == "DEPTH") {
        CVT_TRY_ASSIGN(l.depth, h.number());
      } else if (key == "MAXVAL") {
        CVT_TRY_ASSIGN(l.maxval, h.number());
      } else if (key == "TUPLTYPE") {
        tuple = std::string(util::trim(h.line()));
      } else {
        h.line();
      }
    }
    if (!ended) return fail(Status::Decode, "PAM header has no ENDHDR");
    if (l.depth == 0 || l.depth > 4) return fail(Status::Decode, "PAM depth {} is not supported", l.depth);
    l.alpha = tuple.ends_with("_ALPHA") || l.depth == 2 || l.depth == 4;
    l.tuple = tuple.starts_with("RGB") || l.depth >= 3 ? Tuple::Rgb : tuple.starts_with("BLACKANDWHITE") ? Tuple::BlackWhite : Tuple::Grey;
    return build(l, h, ctx.input, ctx);
  }
  if (kind < '1' || kind > '6') return fail(Status::Decode, "not a Netpbm file");
  CVT_TRY_ASSIGN(l.width, h.number());
  CVT_TRY_ASSIGN(l.height, h.number());
  if (kind == '1' || kind == '4') {
    l.maxval = 1;
    l.tuple = Tuple::BlackWhite;
    l.plain_bits = kind == '1';
    l.packed_bits = kind == '4';
  } else {
    CVT_TRY_ASSIGN(l.maxval, h.number());
    l.tuple = kind == '2' || kind == '5' ? Tuple::Grey : Tuple::Rgb;
    l.depth = l.tuple == Tuple::Rgb ? 3 : 1;
    l.ascii = kind == '2' || kind == '3';
  }
  if (!l.ascii && !l.plain_bits) {
    // Exactly one whitespace byte ends the header of binary variants.
    if (h.at_end() || !util::is_ascii_space(h.peek())) return fail(Status::Decode, "malformed Netpbm header");
    h.advance(1);
  }
  return build(l, h, ctx.input, ctx);
}

// --------------------------------------------------------------------------- writing

Result<Bytes> write_netpbm(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  const std::string& id = ctx.format_id;
  const ir::Image& image = *prepared;
  warn_if_animated(image, ctx, "Netpbm");
  const bool ascii = ctx.options.get_bool("ascii", false);
  const std::uint32_t w = image.width();
  const std::uint32_t h = image.height();
  util::ByteWriter out;

  if (id == "pfm") {
    const ir::HdrPixels pixels = image.hdr && image.hdr->width == w && image.hdr->height == h ? *image.hdr : to_linear(image.frames[0]);
    const bool colour = !is_grayscale(image.frames[0]);
    out.text(std::format("{}\n{} {}\n-1.0\n", colour ? "PF" : "Pf", w, h));
    for (std::uint32_t row = 0; row < h; ++row) {
      const std::uint32_t y = h - 1 - row;
      for (std::uint32_t x = 0; x < w; ++x) {
        const float* p = pixels.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
        if (colour) {
          out.f32le(p[0]).f32le(p[1]).f32le(p[2]);
        } else {
          out.f32le(p[0]);
        }
      }
    }
    return out.take();
  }

  ir::Frame frame = image.frames[0];
  const bool keep_alpha = id == "pam";
  if (!keep_alpha && !is_opaque(frame)) {
    CVT_TRY_ASSIGN(frame, opaque_frame(frame, ctx, "Netpbm"));
  }
  const bool sixteen = id != "pbm" && image.hdr && image.hdr->width == w && image.hdr->height == h &&
                       image.source_bit_depth > 8 && ctx.options.get_enum("bit_depth", "auto", {"auto", "8", "16"}) != "8";
  std::vector<std::uint16_t> wide;
  if (sixteen) wide = srgb16_from_linear(*image.hdr);
  const auto sample = [&](std::size_t pixel, std::size_t c) -> std::uint32_t {
    return sixteen ? wide[pixel * 4 + c] : frame.rgba[pixel * 4 + c];
  };
  const std::uint32_t maxval = sixteen ? 65535 : 255;
  bool grey = is_grayscale(frame);

  if (id == "pbm") {
    ir::Frame bw = frame;
    to_grayscale(bw);
    reduce_bit_depth(bw, 1, resolved_dither(ctx.options));
    out.text(std::format("{}\n{} {}\n", ascii ? "P1" : "P4", w, h));
    for (std::uint32_t y = 0; y < h; ++y) {
      if (ascii) {
        std::string line;
        for (std::uint32_t x = 0; x < w; ++x) line += bw.rgba[bw.offset(x, y)] < 128 ? '1' : '0';
        out.text(line).text("\n");
      } else {
        std::vector<std::uint8_t> row((w + 7) / 8, 0);
        for (std::uint32_t x = 0; x < w; ++x) {
          if (bw.rgba[bw.offset(x, y)] < 128) row[x / 8] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
        }
        out.bytes(std::span<const std::uint8_t>(row));
      }
    }
    return out.take();
  }
  if (id == "pgm" && !grey) {
    to_grayscale(frame);
    grey = true;
    if (sixteen) {
      for (std::size_t i = 0; i + 3 < wide.size(); i += 4) {
        const double y = 0.2126 * srgb_to_linear(wide[i] / 65535.0f) + 0.7152 * srgb_to_linear(wide[i + 1] / 65535.0f) +
                         0.0722 * srgb_to_linear(wide[i + 2] / 65535.0f);
        wide[i] = wide[i + 1] = wide[i + 2] = linear_to_srgb16(static_cast<float>(y));
      }
    }
  }
  const bool write_grey = id == "pgm" || (id == "pam" && grey) || (id == "pnm" && grey);
  std::uint32_t channels = write_grey ? 1 : 3;
  const bool alpha = keep_alpha && !is_opaque(frame);
  if (alpha) ++channels;
  if (id == "pam") {
    const char* tuple = write_grey ? (alpha ? "GRAYSCALE_ALPHA" : "GRAYSCALE") : (alpha ? "RGB_ALPHA" : "RGB");
    out.text(std::format("P7\nWIDTH {}\nHEIGHT {}\nDEPTH {}\nMAXVAL {}\nTUPLTYPE {}\nENDHDR\n", w, h, channels, maxval, tuple));
  } else {
    const char* magic = write_grey ? (ascii ? "P2" : "P5") : (ascii ? "P3" : "P6");
    out.text(std::format("{}\n{} {}\n{}\n", magic, w, h, maxval));
  }
  for (std::uint32_t y = 0; y < h; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / h));
    std::string line;
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t p = static_cast<std::size_t>(y) * w + x;
      for (std::uint32_t c = 0; c < channels; ++c) {
        const std::size_t channel = write_grey ? (c == 0 ? 0 : 3) : (c < 3 ? c : 3);
        const std::uint32_t v = sample(p, channel);
        if (ascii && id != "pam") {
          line += std::to_string(v);
          line += ' ';
        } else if (sixteen) {
          out.u16be(static_cast<std::uint16_t>(v));
        } else {
          out.u8(static_cast<std::uint8_t>(v));
        }
      }
    }
    if (ascii && id != "pam") {
      if (!line.empty()) line.back() = '\n';
      out.text(line);
    }
  }
  return out.take();
}

int sniff_kind(std::span<const std::byte> data, std::string_view kinds) {
  if (data.size() < 3 || std::to_integer<char>(data[0]) != 'P') return 0;
  const char k = std::to_integer<char>(data[1]);
  if (kinds.find(k) == std::string_view::npos) return 0;
  const char next = std::to_integer<char>(data[2]);
  return util::is_ascii_space(next) ? 88 : 0;
}

void add_netpbm(Registry& registry, const char* id, const char* name, const char* description,
                std::vector<std::string> extensions, const char* mime, int popularity, std::string_view kinds) {
  FormatInfo info;
  info.id = id;
  info.name = name;
  info.description = description;
  info.category = Category::Image;
  info.extensions = std::move(extensions);
  info.mime = mime;
  info.popularity = popularity;
  const std::string kind_list(kinds);
  info.sniff = [kind_list](std::span<const std::byte> data, std::string_view) { return sniff_kind(data, kind_list); };
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = id;
  spec.read = reader(read_pnm);
  spec.write = writer(write_netpbm);
  spec.write_options = plain_writer_options();
  const std::string_view sid(id);
  if (sid == "pbm" || sid == "pgm" || sid == "ppm" || sid == "pnm") {
    spec.write_options.push_back(OptionSpec::boolean("ascii", "Plain text", false,
                                                     "Write the human-readable ASCII variant instead of binary."));
  }
  if (sid == "pbm") spec.write_options.push_back(dither_option());
  if (sid != "pbm" && sid != "pfm") {
    spec.write_options.push_back(OptionSpec::choice("bit_depth", "Bit depth", "auto", {"auto", "8", "16"},
                                                    "Bits per sample; auto keeps 16 bits for 16-bit sources.")
                                     .advanced());
  }
  if (sid != "pam" && sid != "pfm") spec.write_options.push_back(background_option());
  if (sid == "pfm") {
    spec.read_options.push_back(OptionSpec::number("exposure", "Exposure", 0.0, -16.0, 16.0,
                                                   "Brightness adjustment in stops when mapping HDR to normal range.")
                                    .advanced());
  }
  add_image_codec(registry, std::move(spec));
}

}  // namespace

void register_pnm(Registry& registry) {
  add_netpbm(registry, "pbm", "Portable bitmap", "Netpbm black-and-white image.", {"pbm"}, "image/x-portable-bitmap", 15,
             "14");
  add_netpbm(registry, "pgm", "Portable graymap", "Netpbm greyscale image.", {"pgm"}, "image/x-portable-graymap", 15,
             "25");
  add_netpbm(registry, "ppm", "Portable pixmap", "Netpbm colour image.", {"ppm"}, "image/x-portable-pixmap", 20, "36");
  add_netpbm(registry, "pnm", "Portable anymap", "Any Netpbm image.", {"pnm"}, "image/x-portable-anymap", 10, "");
  add_netpbm(registry, "pam", "Portable arbitrary map", "Netpbm image with optional transparency.", {"pam"},
             "image/x-portable-arbitrarymap", 10, "7");
  add_netpbm(registry, "pfm", "Portable float map", "32-bit floating point HDR image.", {"pfm"}, "image/x-portable-floatmap",
             8, "Ff");
}

}  // namespace cvt::image
