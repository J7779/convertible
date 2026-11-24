// Convertible core - image family: Apple icon images (ICNS).
//
// Reader: PNG entries (icp4/5/6, ic07-ic14), JPEG 2000 entries when a jp2
// decoder is registered, run-length RGB entries (is32/il32/ih32/it32) with
// their 8-bit masks (s8mk/l8mk/h8mk/t8mk), ARGB entries (ic04/ic05) and the
// classic 1-bit icons with masks (ics#, ICN#, ich#); the largest entry wins.
// Writer: PNG-based entries for 16 ... 1024 pixels (never enlarging).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "families/image/png_codec.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

struct Chunk {
  std::string type;
  ByteView data;
};

struct Kind {
  std::string_view type;
  std::uint32_t size;
  int encoding;  // 0 PNG/JP2, 1 RLE RGB, 2 ARGB, 3 one-bit with mask
  std::string_view mask;
};

constexpr Kind kinds[] = {
    {"ic10", 1024, 0, ""}, {"ic14", 512, 0, ""},  {"ic09", 512, 0, ""},  {"ic13", 256, 0, ""}, {"ic08", 256, 0, ""},
    {"ic07", 128, 0, ""},  {"it32", 128, 1, "t8mk"}, {"ic12", 64, 0, ""}, {"icp6", 64, 0, ""}, {"ih32", 48, 1, "h8mk"},
    {"ic11", 32, 0, ""},   {"icp5", 32, 0, ""},  {"il32", 32, 1, "l8mk"}, {"ic05", 32, 2, ""}, {"ICN#", 32, 3, ""},
    {"icp4", 16, 0, ""},   {"is32", 16, 1, "s8mk"}, {"ic04", 16, 2, ""}, {"ics#", 16, 3, ""},  {"ich#", 48, 3, ""},
};

/// Apple's PackBits variant used by icon channels.
bool unpack_channel(util::ByteReader& r, std::uint8_t* out, std::size_t count, std::size_t stride) {
  std::size_t i = 0;
  while (i < count) {
    const std::uint8_t control = r.u8();
    if (!r.ok()) return false;
    if (control < 128) {
      const std::size_t n = std::size_t{control} + 1;
      for (std::size_t k = 0; k < n && i < count; ++k) out[(i++) * stride] = r.u8();
    } else {
      const std::size_t n = std::size_t{control} - 125;
      const std::uint8_t value = r.u8();
      for (std::size_t k = 0; k < n && i < count; ++k) out[(i++) * stride] = value;
    }
    if (!r.ok()) return false;
  }
  return true;
}

std::optional<ir::Frame> decode_entry(const Kind& kind, const Chunk& chunk, const std::vector<Chunk>& all,
                                      const Context& ctx) {
  const std::uint32_t n = kind.size;
  if (kind.encoding == 0) {
    const std::string_view head = as_chars(chunk.data.first(std::min<std::size_t>(chunk.data.size(), 12)));
    const bool png_data = head.starts_with("\x89PNG");
    auto image = png_data ? png::decode(chunk.data, ctx) : decode("jp2", chunk.data, ctx);
    if (!image) return std::nullopt;
    return std::move(image->frames[0]);
  }
  ir::Frame frame = ir::Frame::filled(n, n, {0, 0, 0, 255});
  const std::size_t pixels = std::size_t{n} * n;
  if (kind.encoding == 1) {
    util::ByteReader r(chunk.data);
    if (kind.type == "it32") r.skip(4);
    const bool raw = chunk.data.size() == pixels * 3 || chunk.data.size() == pixels * 4;
    if (raw) {
      // Some writers store small icons uncompressed (ARGB or RGB).
      const std::size_t channels = chunk.data.size() / pixels;
      for (std::size_t i = 0; i < pixels; ++i) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(chunk.data.data()) + i * channels + (channels == 4 ? 1 : 0);
        ::cvt::util::copy_bytes(frame.rgba.data() + i * 4, p, 3);
      }
    } else {
      for (std::size_t c = 0; c < 3; ++c) {
        if (!unpack_channel(r, frame.rgba.data() + c, pixels, 4)) return std::nullopt;
      }
    }
    for (const Chunk& m : all) {
      if (m.type == kind.mask && m.data.size() >= pixels) {
        for (std::size_t i = 0; i < pixels; ++i) frame.rgba[i * 4 + 3] = std::to_integer<std::uint8_t>(m.data[i]);
      }
    }
    return frame;
  }
  if (kind.encoding == 2) {
    if (chunk.data.size() >= 8 && as_chars(chunk.data.first(4)) == "\x89PNG") {
      auto image = png::decode(chunk.data, ctx);
      if (!image) return std::nullopt;
      return std::move(image->frames[0]);
    }
    if (chunk.data.size() < 4 || as_chars(chunk.data.first(4)) != "ARGB") return std::nullopt;
    util::ByteReader r(chunk.data.subspan(4));
    const std::size_t order[4] = {3, 0, 1, 2};
    for (const std::size_t c : order) {
      if (!unpack_channel(r, frame.rgba.data() + c, pixels, 4)) return std::nullopt;
    }
    return frame;
  }
  // One-bit icon followed by its one-bit mask.
  const std::size_t bytes = pixels / 8;
  if (chunk.data.size() < bytes * 2) return std::nullopt;
  for (std::size_t i = 0; i < pixels; ++i) {
    const bool black = (std::to_integer<std::uint8_t>(chunk.data[i / 8]) >> (7 - i % 8)) & 1u;
    const bool opaque = (std::to_integer<std::uint8_t>(chunk.data[bytes + i / 8]) >> (7 - i % 8)) & 1u;
    const std::uint8_t v = black ? 0 : 255;
    frame.rgba[i * 4] = frame.rgba[i * 4 + 1] = frame.rgba[i * 4 + 2] = v;
    frame.rgba[i * 4 + 3] = opaque ? 255 : 0;
  }
  return frame;
}

Result<ir::Image> read_icns(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("icns")) return fail(Status::Decode, "not an ICNS file");
  const std::uint32_t total = r.u32be();
  if (!r.ok() || total < 8) return fail(Status::Decode, "ICNS header is invalid");
  std::vector<Chunk> chunks;
  const std::size_t end = std::min<std::size_t>(total, ctx.input.size());
  while (r.position() + 8 <= end) {
    Chunk c;
    c.type = std::string(r.chars(4));
    const std::uint32_t length = r.u32be();
    if (length < 8 || length - 8u > end - r.position()) break;
    c.data = r.bytes(length - 8u);
    chunks.push_back(std::move(c));
    if (chunks.size() > 1024) break;
  }
  for (const Kind& kind : kinds) {
    for (const Chunk& chunk : chunks) {
      if (chunk.type != kind.type) continue;
      auto frame = decode_entry(kind, chunk, chunks, ctx);
      if (!frame || !frame->valid()) continue;
      ir::Image image;
      image.frames.push_back(std::move(*frame));
      return image;
    }
  }
  return fail(Status::Decode, "the ICNS file has no entry that can be decoded");
}

Result<ir::Frame> square(const ir::Frame& source, std::uint32_t size) {
  const double scale = std::min(static_cast<double>(size) / source.width, static_cast<double>(size) / source.height);
  const auto w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(source.width * scale)));
  const auto h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(source.height * scale)));
  CVT_TRY_ASSIGN(const ir::Frame scaled, resize(source, w, h));
  ir::Frame out = ir::Frame::filled(size, size, {0, 0, 0, 0});
  for (std::uint32_t y = 0; y < h; ++y) {
    ::cvt::util::copy_bytes(out.rgba.data() + out.offset((size - w) / 2, (size - h) / 2 + y), scaled.rgba.data() + scaled.offset(0, y),
                std::size_t{w} * 4);
  }
  return out;
}

Result<Bytes> write_icns(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "ICNS");
  const ir::Frame& frame = prepared->frames.front();
  const std::uint32_t largest = std::max(frame.width, frame.height);
  struct Target {
    std::uint32_t size;
    const char* type;
  };
  constexpr Target targets[] = {{16, "icp4"}, {32, "icp5"}, {64, "icp6"}, {128, "ic07"},
                                {256, "ic08"}, {512, "ic09"}, {1024, "ic10"}};
  util::ByteWriter body;
  int written = 0;
  for (const Target& t : targets) {
    if (t.size > largest && written > 0) break;
    CVT_TRY(ctx.checkpoint(static_cast<double>(written) / 7.0, "Encoding"));
    CVT_TRY_ASSIGN(const ir::Frame icon, square(frame, t.size));
    ir::Image one;
    one.frames.push_back(icon);
    png::EncodeOptions options;
    options.compression_level = 9;
    CVT_TRY_ASSIGN(const Bytes png, png::encode(one, options, ctx));
    body.text(t.type).u32be(static_cast<std::uint32_t>(png.size() + 8)).bytes(png);
    ++written;
  }
  util::ByteWriter out;
  out.text("icns").u32be(static_cast<std::uint32_t>(body.size() + 8)).bytes(body.view());
  return out.take();
}

}  // namespace

void register_icns(Registry& registry) {
  FormatInfo info;
  info.id = "icns";
  info.name = "Apple icon image";
  info.description = "macOS application icon with several sizes.";
  info.category = Category::Image;
  info.extensions = {"icns"};
  info.mime = "image/icns";
  info.popularity = 30;
  info.signatures = {Signature::at(0, "'icns'")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "icns";
  spec.read = reader(read_icns);
  spec.write = writer(write_icns);
  spec.write_options = plain_writer_options();
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
