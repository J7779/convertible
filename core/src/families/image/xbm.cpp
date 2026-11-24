// Convertible core - image family: X BitMap (XBM, X10 and X11 forms).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
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

/// Value of "#define <name>_<suffix> <n>" anywhere in the text.
std::optional<std::uint32_t> define_value(std::string_view text, std::string_view suffix) {
  std::size_t at = 0;
  while ((at = text.find("#define", at)) != std::string_view::npos) {
    at += 7;
    std::size_t p = at;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    const std::size_t name_start = p;
    while (p < text.size() && !util::is_ascii_space(text[p])) ++p;
    const std::string_view name = text.substr(name_start, p - name_start);
    if (!name.ends_with(suffix)) continue;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    const std::size_t v = p;
    while (p < text.size() && util::is_ascii_digit(text[p])) ++p;
    const auto value = util::parse_int(text.substr(v, p - v));
    if (value && *value > 0 && *value <= 0x7FFFFFFF) return static_cast<std::uint32_t>(*value);
  }
  return std::nullopt;
}

Result<ir::Image> read_xbm(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  const auto width = define_value(text, "_width");
  const auto height = define_value(text, "_height");
  if (!width || !height) return fail(Status::Decode, "not an XBM file (width and height definitions are missing)");
  CVT_TRY(check_size(ctx, *width, *height));
  const std::size_t open = text.find('{');
  if (open == std::string_view::npos) return fail(Status::Decode, "XBM bitmap data is missing");
  const bool x10 = text.find("short") != std::string_view::npos && text.find("short") < open;
  const std::size_t unit = x10 ? 16 : 8;
  const std::size_t row_units = (*width + unit - 1) / unit;
  CVT_TRY(check_plausible(std::uint64_t{*width} * *height, text.size(), 4));
  std::vector<std::uint16_t> values;
  values.reserve(row_units * *height);
  std::size_t p = open + 1;
  while (p < text.size() && values.size() < row_units * *height) {
    const std::size_t start = text.find("0x", p);
    const std::size_t close = text.find('}', p);
    if (start == std::string_view::npos || (close != std::string_view::npos && close < start)) break;
    std::size_t e = start + 2;
    const auto is_hex = [](char c) {
      return util::is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    while (e < text.size() && is_hex(text[e])) ++e;
    const auto v = util::parse_int(text.substr(start + 2, e - start - 2), 16);
    if (!v || *v < 0 || *v > 0xFFFF) return fail(Status::Decode, "XBM bitmap data is malformed");
    values.push_back(static_cast<std::uint16_t>(*v));
    p = e;
  }
  if (values.size() < row_units * *height) {
    if (values.empty()) return fail(Status::Decode, "XBM bitmap data is missing");
    ctx.warn("The XBM data is incomplete; missing rows were left white.");
    values.resize(row_units * *height, 0);
  }
  ir::Frame frame = ir::Frame::filled(*width, *height, {255, 255, 255, 255});
  for (std::uint32_t y = 0; y < *height; ++y) {
    for (std::uint32_t x = 0; x < *width; ++x) {
      const std::uint16_t v = values[y * row_units + x / unit];
      if ((v >> (x % unit)) & 1u) {  // least significant bit first, 1 = foreground (black)
        std::uint8_t* px = frame.rgba.data() + frame.offset(x, y);
        px[0] = px[1] = px[2] = 0;
      }
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  image.source_bit_depth = 1;
  image.source_grayscale = true;
  return image;
}

std::string identifier_from(std::string_view filename) {
  std::string name;
  const std::size_t slash = filename.find_last_of("/\\");
  std::string_view base = slash == std::string_view::npos ? filename : filename.substr(slash + 1);
  const std::size_t dot = base.find('.');
  if (dot != std::string_view::npos) base = base.substr(0, dot);
  for (const char c : base) name += (util::is_ascii_alpha(c) || util::is_ascii_digit(c)) ? c : '_';
  if (name.empty() || util::is_ascii_digit(name[0])) name = "image" + name;
  return name;
}

Result<Bytes> write_xbm(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "XBM");
  CVT_TRY_ASSIGN(ir::Frame frame, opaque_frame(prepared->frames.front(), ctx, "XBM"));
  to_grayscale(frame);
  reduce_bit_depth(frame, 1, resolved_dither(ctx.options));
  const std::string name = identifier_from(ctx.source_filename);
  std::string out = std::format("#define {0}_width {1}\n#define {0}_height {2}\nstatic unsigned char {0}_bits[] = {{\n",
                                name, frame.width, frame.height);
  const std::size_t row = (frame.width + 7) / 8;
  std::size_t count = 0;
  const std::size_t total = row * frame.height;
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    for (std::size_t b = 0; b < row; ++b) {
      std::uint32_t v = 0;
      for (std::uint32_t k = 0; k < 8; ++k) {
        const std::uint32_t x = static_cast<std::uint32_t>(b * 8 + k);
        if (x < frame.width && frame.rgba[frame.offset(x, y)] < 128) v |= 1u << k;
      }
      out += count % 12 == 0 ? "   " : " ";
      out += std::format("0x{:02x}", v);
      ++count;
      if (count < total) out += ',';
      if (count % 12 == 0 || count == total) out += '\n';
    }
  }
  out += "};\n";
  return to_bytes(out);
}

int sniff_xbm(std::span<const std::byte> data, std::string_view /*filename*/) {
  const std::string_view text = as_chars(data.first(std::min<std::size_t>(data.size(), 4096)));
  if (!text.starts_with("#define") && text.find("\n#define") == std::string_view::npos) return 0;
  if (text.find("_width") == std::string_view::npos || text.find("_bits[]") == std::string_view::npos) return 0;
  return 90;
}

}  // namespace

void register_xbm(Registry& registry) {
  FormatInfo info;
  info.id = "xbm";
  info.name = "X BitMap";
  info.description = "Black-and-white X11 image stored as C source.";
  info.category = Category::Image;
  info.extensions = {"xbm"};
  info.mime = "image/x-xbitmap";
  info.popularity = 5;
  info.sniff = sniff_xbm;
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "xbm";
  spec.read = reader(read_xbm);
  spec.write = writer(write_xbm);
  spec.write_options = plain_writer_options();
  spec.write_options.push_back(dither_option());
  spec.write_options.push_back(background_option());
  spec.lossy_write = true;
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
