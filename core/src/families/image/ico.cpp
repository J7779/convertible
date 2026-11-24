// Convertible core - image family: Windows icons and cursors (ICO, CUR).
//
// Reader: every entry kind (1/4/8/24/32-bit DIBs with their AND masks,
// PNG-compressed entries); the largest, deepest entry becomes the picture.
// Writer: a multi-size icon set (16 ... 256 pixels by default, never
// enlarging), small sizes as 32-bit DIBs with masks for compatibility,
// 64 pixels and up as PNG; cursors carry a hotspot.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/bmp_codec.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "families/image/png_codec.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

constexpr std::string_view png_magic("\x89PNG\r\n\x1A\n", 8);

struct DirEntry {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t bpp = 0;
  std::uint32_t size = 0;
  std::uint32_t offset = 0;
};

/// Apply the 1-bit AND mask that follows the colour bitmap of a DIB entry.
void apply_and_mask(ByteView dib, ir::Frame& frame) {
  util::ByteReader r(dib);
  const std::uint32_t header = r.u32le();
  if (header < 40) return;
  r.skip(8);
  r.skip(2);
  const std::uint16_t bpp = r.u16le();
  const std::uint32_t compression = r.u32le();
  r.skip(12);
  const std::uint32_t used = r.u32le();
  if (!r.ok()) return;
  const std::uint64_t palette = bpp <= 8 ? (used != 0 ? used : (1u << bpp)) * 4u : 0u;
  const std::uint64_t masks = compression == 3 && header == 40 ? 12 : 0;
  const std::uint32_t w = frame.width;
  const std::uint32_t h = frame.height;
  const std::uint64_t colour_stride = ((std::uint64_t{w} * bpp + 31) / 32) * 4;
  const std::uint64_t mask_stride = ((std::uint64_t{w} + 31) / 32) * 4;
  const std::uint64_t start = header + palette + masks + colour_stride * h;
  if (start + mask_stride * h > dib.size()) return;  // no mask stored
  bool alpha_used = false;
  if (bpp == 32) {
    for (std::size_t i = 3; i < frame.rgba.size(); i += 4) alpha_used = alpha_used || frame.rgba[i] != 255;
  }
  if (alpha_used) return;  // 32-bit icons carry real alpha; the mask is only a fallback
  for (std::uint32_t row = 0; row < h; ++row) {
    const std::uint32_t y = h - 1 - row;
    const auto* bits = reinterpret_cast<const std::uint8_t*>(dib.data()) + start + mask_stride * row;
    for (std::uint32_t x = 0; x < w; ++x) {
      if ((bits[x / 8] >> (7 - x % 8)) & 1u) {
        ::cvt::util::fill_bytes(frame.rgba.data() + frame.offset(x, y), 0, 4);
      }
    }
  }
}

Result<ir::Image> read_icon(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const std::uint16_t reserved = r.u16le();
  const std::uint16_t type = r.u16le();
  const std::uint16_t count = r.u16le();
  if (!r.ok() || reserved != 0 || (type != 1 && type != 2) || count == 0) {
    return fail(Status::Decode, "not an icon or cursor file");
  }
  std::vector<DirEntry> entries;
  for (std::uint16_t i = 0; i < count; ++i) {
    DirEntry e;
    const std::uint8_t w = r.u8();
    const std::uint8_t h = r.u8();
    r.skip(2);  // colours, reserved
    r.skip(2);  // planes / hotspot x
    e.bpp = r.u16le();
    e.size = r.u32le();
    e.offset = r.u32le();
    if (!r.ok()) break;
    e.width = w == 0 ? 256 : w;
    e.height = h == 0 ? 256 : h;
    if (e.offset >= ctx.input.size() || e.size == 0) continue;
    e.size = static_cast<std::uint32_t>(std::min<std::uint64_t>(e.size, ctx.input.size() - e.offset));
    entries.push_back(e);
  }
  if (entries.empty()) return fail(Status::Decode, "the icon file has no readable entries");
  // Largest first, then the deepest colour.
  std::stable_sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
    if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
    return a.bpp > b.bpp;
  });
  Error last{Status::Decode, "no icon entry could be decoded"};
  for (const DirEntry& e : entries) {
    const ByteView data = ctx.input.subspan(e.offset, e.size);
    if (data.size() >= 8 && as_chars(data.first(8)) == png_magic) {
      auto image = png::decode(data, ctx);
      if (image) return std::move(*image);
      last = image.error();
      continue;
    }
    auto image = bmp::decode_dib(data, 0, ctx, true);
    if (!image) {
      last = image.error();
      continue;
    }
    apply_and_mask(data, image->frames[0]);
    return std::move(*image);
  }
  return fail(std::move(last));
}

// --------------------------------------------------------------------------- writing

std::vector<std::uint32_t> parse_sizes(const std::string& text, std::uint32_t source_max, bool& error) {
  std::vector<std::uint32_t> sizes;
  error = false;
  if (text.empty() || util::iequals(text, "auto")) {
    for (const std::uint32_t s : {16u, 24u, 32u, 48u, 64u, 128u, 256u}) {
      if (s <= source_max) sizes.push_back(s);
    }
    if (sizes.empty()) sizes.push_back(std::max<std::uint32_t>(1, std::min<std::uint32_t>(source_max, 256)));
    return sizes;
  }
  for (const std::string_view part : util::split(text, ',', false)) {
    const auto value = util::parse_int(util::trim(part));
    if (!value || *value < 1 || *value > 256) {
      error = true;
      return {};
    }
    sizes.push_back(static_cast<std::uint32_t>(*value));
  }
  std::sort(sizes.begin(), sizes.end());
  sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
  return sizes;
}

/// Fit the picture into a size x size square on a transparent canvas.
Result<ir::Frame> square(const ir::Frame& source, std::uint32_t size) {
  const double scale = std::min(static_cast<double>(size) / source.width, static_cast<double>(size) / source.height);
  const auto w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(source.width * scale)));
  const auto h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(source.height * scale)));
  CVT_TRY_ASSIGN(const ir::Frame scaled, resize(source, w, h));
  ir::Frame out = ir::Frame::filled(size, size, {0, 0, 0, 0});
  const std::uint32_t x0 = (size - w) / 2;
  const std::uint32_t y0 = (size - h) / 2;
  for (std::uint32_t y = 0; y < h; ++y) {
    ::cvt::util::copy_bytes(out.rgba.data() + out.offset(x0, y0 + y), scaled.rgba.data() + scaled.offset(0, y), std::size_t{w} * 4);
  }
  return out;
}

Bytes dib_entry(const ir::Frame& frame) {
  util::ByteWriter w;
  const std::uint32_t size = frame.width;
  const std::uint64_t mask_stride = ((std::uint64_t{size} + 31) / 32) * 4;
  w.u32le(40).i32le(static_cast<std::int32_t>(size)).i32le(static_cast<std::int32_t>(size * 2)).u16le(1).u16le(32);
  w.u32le(0).u32le(static_cast<std::uint32_t>(std::uint64_t{size} * size * 4 + mask_stride * size)).u32le(0).u32le(0);
  w.u32le(0).u32le(0);
  for (std::uint32_t row = 0; row < size; ++row) {
    const std::uint32_t y = size - 1 - row;
    for (std::uint32_t x = 0; x < size; ++x) {
      const std::uint8_t* p = frame.rgba.data() + frame.offset(x, y);
      w.u8(p[2]).u8(p[1]).u8(p[0]).u8(p[3]);
    }
  }
  for (std::uint32_t row = 0; row < size; ++row) {
    const std::uint32_t y = size - 1 - row;
    std::vector<std::uint8_t> bits(static_cast<std::size_t>(mask_stride), 0);
    for (std::uint32_t x = 0; x < size; ++x) {
      if (frame.rgba[frame.offset(x, y) + 3] == 0) bits[x / 8] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
    }
    w.bytes(std::span<const std::uint8_t>(bits));
  }
  return w.take();
}

Result<Bytes> write_icon(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  const bool cursor = ctx.format_id == "cur";
  warn_if_animated(*prepared, ctx, cursor ? "A cursor" : "An icon");
  const ir::Frame& frame = prepared->frames.front();
  bool error = false;
  const std::vector<std::uint32_t> sizes =
      parse_sizes(ctx.options.get_string("sizes", "auto"), std::max(frame.width, frame.height), error);
  if (error) return fail(Status::InvalidArgument, "option 'sizes' must list sizes from 1 to 256, such as 16,32,48");
  const std::string encoding = ctx.options.get_enum("entry_format", "auto", {"auto", "png", "bmp"});
  const int hotspot_x = static_cast<int>(ctx.options.get_int("hotspot_x", 0, 0, 255));
  const int hotspot_y = static_cast<int>(ctx.options.get_int("hotspot_y", 0, 0, 255));
  std::vector<Bytes> blobs;
  for (const std::uint32_t size : sizes) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(blobs.size()) / static_cast<double>(sizes.size()), "Encoding"));
    CVT_TRY_ASSIGN(const ir::Frame icon, square(frame, size));
    const bool as_png = encoding == "png" || (encoding == "auto" && size >= 64);
    if (as_png) {
      ir::Image one;
      one.frames.push_back(icon);
      png::EncodeOptions options;
      options.compression_level = 9;
      CVT_TRY_ASSIGN(Bytes encoded, png::encode(one, options, ctx));
      blobs.push_back(std::move(encoded));
    } else {
      blobs.push_back(dib_entry(icon));
    }
  }
  util::ByteWriter out;
  out.u16le(0).u16le(cursor ? 2 : 1).u16le(static_cast<std::uint16_t>(blobs.size()));
  std::uint64_t offset = 6 + 16 * blobs.size();
  for (std::size_t i = 0; i < blobs.size(); ++i) {
    const std::uint32_t size = sizes[i];
    out.u8(static_cast<std::uint8_t>(size >= 256 ? 0 : size)).u8(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
    out.u8(0).u8(0);
    if (cursor) {
      out.u16le(static_cast<std::uint16_t>(hotspot_x * static_cast<int>(size) / 256))
          .u16le(static_cast<std::uint16_t>(hotspot_y * static_cast<int>(size) / 256));
    } else {
      out.u16le(1).u16le(32);
    }
    out.u32le(static_cast<std::uint32_t>(blobs[i].size())).u32le(static_cast<std::uint32_t>(offset));
    offset += blobs[i].size();
  }
  for (const Bytes& blob : blobs) out.bytes(blob);
  return out.take();
}

int sniff_icon(std::span<const std::byte> data, std::string_view /*filename*/, int type) {
  util::ByteReader r(data);
  if (r.u16le() != 0 || r.u16le() != type) return 0;
  const std::uint16_t count = r.u16le();
  if (count == 0 || count > 512 || !r.ok()) return 0;
  // Check the first directory entry for sanity.
  r.skip(8);
  const std::uint32_t size = r.u32le();
  const std::uint32_t offset = r.u32le();
  if (!r.ok() || size == 0 || offset < 6u + 16u * count) return 0;
  return 85;
}

}  // namespace

void register_ico(Registry& registry) {
  FormatInfo ico;
  ico.id = "ico";
  ico.name = "Windows icon";
  ico.description = "Icon with several sizes (favicon, application icon).";
  ico.category = Category::Image;
  ico.extensions = {"ico"};
  ico.mime = "image/vnd.microsoft.icon";
  ico.popularity = 50;
  ico.sniff = [](std::span<const std::byte> d, std::string_view f) { return sniff_icon(d, f, 1); };
  registry.add_format(std::move(ico));

  FormatInfo cur;
  cur.id = "cur";
  cur.name = "Windows cursor";
  cur.description = "Mouse pointer image with a hotspot.";
  cur.category = Category::Image;
  cur.extensions = {"cur"};
  cur.mime = "image/x-win-bitmap";
  cur.popularity = 15;
  cur.sniff = [](std::span<const std::byte> d, std::string_view f) { return sniff_icon(d, f, 2); };
  registry.add_format(std::move(cur));

  for (const char* id : {"ico", "cur"}) {
    ImageCodecSpec spec;
    spec.format_id = id;
    spec.read = reader(read_icon);
    spec.write = writer(write_icon);
    spec.write_options = plain_writer_options();
    spec.write_options.push_back(OptionSpec::text("sizes", "Sizes", "auto",
                                                  "Comma-separated edge lengths to include, such as 16,32,48,256; "
                                                  "auto uses 16 to 256 without enlarging."));
    spec.write_options.push_back(OptionSpec::choice("entry_format", "Entry format", "auto", {"auto", "png", "bmp"},
                                                    "How each size is stored: auto uses PNG from 64 pixels up and "
                                                    "bitmaps below.")
                                     .advanced());
    if (std::string_view(id) == "cur") {
      spec.write_options.push_back(OptionSpec::integer("hotspot_x", "Hotspot X", 0, 0, 255,
                                                       "Pointer position from the left, in pixels of a 256-pixel cursor."));
      spec.write_options.push_back(OptionSpec::integer("hotspot_y", "Hotspot Y", 0, 0, 255,
                                                       "Pointer position from the top, in pixels of a 256-pixel cursor."));
    }
    add_image_codec(registry, std::move(spec));
  }
}

}  // namespace cvt::image
