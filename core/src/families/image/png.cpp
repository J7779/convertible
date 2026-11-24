// Convertible core - image family: PNG and APNG (ISO/IEC 15948, APNG 1.0),
// plus the "png" intra-only video codec.
//
// Reader: every colour type and bit depth, Adam7, tRNS, PLTE, gAMA (applied
// unless sRGB/iCCP say the data is sRGB already), iCCP, pHYs, tEXt/zTXt/iTXt,
// eXIf, APNG animation (acTL/fcTL/fdAT with all dispose and blend modes).
// 16-bit sources keep full precision in `ir::Image::hdr`.
//
// Writer: lossless by default with automatic colour type and bit depth
// reduction (grey, palette, 1/2/4-bit, tRNS colour keys), adaptive row
// filters and DEFLATE; a "small" path that quantises to a palette when a
// lossy quality is requested; APNG with frame differencing for animations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec/codec.hpp"
#include "codec/colorspace.hpp"
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "families/image/png_codec.hpp"
#include "util/bytes.hpp"
#include "util/crc.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::image::png {

namespace {

using namespace std::string_view_literals;

constexpr std::string_view signature = "\x89PNG\r\n\x1A\n"sv;

struct Header {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int bit_depth = 8;
  int color_type = 0;
  bool interlaced = false;

  [[nodiscard]] int channels() const noexcept {
    switch (color_type) {
      case 0: return 1;
      case 2: return 3;
      case 3: return 1;
      case 4: return 2;
      default: return 4;
    }
  }
  [[nodiscard]] int bits_per_pixel() const noexcept { return channels() * bit_depth; }
  /// Bytes per complete pixel, at least 1 (the filter "bpp").
  [[nodiscard]] std::size_t filter_bpp() const noexcept {
    return std::max<std::size_t>(1, static_cast<std::size_t>(bits_per_pixel()) / 8);
  }
  [[nodiscard]] std::uint64_t row_bytes(std::uint64_t width_px) const noexcept {
    return (width_px * static_cast<std::uint64_t>(bits_per_pixel()) + 7) / 8;
  }
};

struct Pass {
  std::uint32_t x0, y0, dx, dy;
};
constexpr std::array<Pass, 7> adam7 = {{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4},
                                        {1, 0, 2, 2}, {0, 1, 1, 2}}};

std::uint32_t pass_extent(std::uint32_t size, std::uint32_t start, std::uint32_t step) {
  return size > start ? (size - start + step - 1) / step : 0;
}

/// Total size of the filtered (raw) data for a header.
std::uint64_t raw_size(const Header& h) {
  if (!h.interlaced) return (h.row_bytes(h.width) + 1) * h.height;
  std::uint64_t total = 0;
  for (const Pass& p : adam7) {
    const std::uint32_t w = pass_extent(h.width, p.x0, p.dx);
    const std::uint32_t rows = pass_extent(h.height, p.y0, p.dy);
    if (w == 0 || rows == 0) continue;
    total += (h.row_bytes(w) + 1) * rows;
  }
  return total;
}

inline std::uint8_t paeth(int a, int b, int c) noexcept {
  const int p = a + b - c;
  const int pa = std::abs(p - a);
  const int pb = std::abs(p - b);
  const int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
  if (pb <= pc) return static_cast<std::uint8_t>(b);
  return static_cast<std::uint8_t>(c);
}

/// Undo the filter of one row in place. `prior` is the unfiltered previous
/// row (all zeros for the first row of a pass).
Result<void> unfilter_row(int filter, std::uint8_t* row, const std::uint8_t* prior, std::size_t length,
                          std::size_t bpp) {
  switch (filter) {
    case 0: return {};
    case 1:
      for (std::size_t i = bpp; i < length; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
      return {};
    case 2:
      for (std::size_t i = 0; i < length; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prior[i]);
      return {};
    case 3:
      for (std::size_t i = 0; i < bpp && i < length; ++i) row[i] = static_cast<std::uint8_t>(row[i] + (prior[i] >> 1));
      for (std::size_t i = bpp; i < length; ++i) {
        row[i] = static_cast<std::uint8_t>(row[i] + ((row[i - bpp] + prior[i]) >> 1));
      }
      return {};
    case 4:
      for (std::size_t i = 0; i < bpp && i < length; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prior[i]);
      for (std::size_t i = bpp; i < length; ++i) {
        row[i] = static_cast<std::uint8_t>(row[i] + paeth(row[i - bpp], prior[i], prior[i - bpp]));
      }
      return {};
    default: return fail(Status::Decode, "PNG row uses unknown filter type {}", filter);
  }
}

// --------------------------------------------------------------------------- decoding

struct Palette {
  std::vector<Rgba> entries;
};

struct Transparency {
  bool present = false;
  std::uint16_t key[3] = {0, 0, 0};  // grey in [0], or R, G, B
};

/// Decoded pixels of one image (still picture or APNG frame region).
struct Pixels {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba8;    // always filled
  std::vector<std::uint16_t> rgba16;  // 16-bit sources only (when requested)
};

struct DecodeState {
  Header header;
  Palette palette;
  Transparency trns;
  bool want16 = false;
};

/// Expand one unfiltered row of `count` pixels into RGBA, writing pixel i to
/// column `x0 + i * dx` of row `y` in `out`.
void expand_row(const DecodeState& state, const std::uint8_t* row, std::uint32_t count, std::uint32_t x0,
                std::uint32_t dx, std::uint32_t y, Pixels& out) {
  const Header& h = state.header;
  const int depth = h.bit_depth;
  std::uint8_t* dst8 = out.rgba8.data() + static_cast<std::size_t>(y) * out.width * 4;
  std::uint16_t* dst16 = out.rgba16.empty() ? nullptr : out.rgba16.data() + static_cast<std::size_t>(y) * out.width * 4;
  const auto put = [&](std::uint32_t i, std::uint16_t r, std::uint16_t g, std::uint16_t b, std::uint16_t a) {
    const std::size_t at = static_cast<std::size_t>(x0 + i * dx) * 4;
    if (depth == 16) {
      dst8[at] = static_cast<std::uint8_t>((r * 255u + 32767u) / 65535u);
      dst8[at + 1] = static_cast<std::uint8_t>((g * 255u + 32767u) / 65535u);
      dst8[at + 2] = static_cast<std::uint8_t>((b * 255u + 32767u) / 65535u);
      dst8[at + 3] = static_cast<std::uint8_t>((a * 255u + 32767u) / 65535u);
      if (dst16 != nullptr) {
        dst16[at] = r;
        dst16[at + 1] = g;
        dst16[at + 2] = b;
        dst16[at + 3] = a;
      }
    } else {
      dst8[at] = static_cast<std::uint8_t>(r);
      dst8[at + 1] = static_cast<std::uint8_t>(g);
      dst8[at + 2] = static_cast<std::uint8_t>(b);
      dst8[at + 3] = static_cast<std::uint8_t>(a);
    }
  };
  const auto sample16 = [&](std::size_t index) {
    return static_cast<std::uint16_t>(row[index * 2] << 8 | row[index * 2 + 1]);
  };
  switch (h.color_type) {
    case 0: {  // grey
      if (depth == 16) {
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint16_t v = sample16(i);
          const std::uint16_t a = state.trns.present && v == state.trns.key[0] ? 0 : 65535;
          put(i, v, v, v, a);
        }
      } else {
        const int per_byte = 8 / depth;
        const std::uint32_t mask = (1u << depth) - 1u;
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint32_t raw =
              depth < 8 ? (row[i / per_byte] >> (8 - depth - static_cast<int>(i % per_byte) * depth)) & mask : row[i];
          const std::uint8_t v = scale_to_8(raw, depth);
          const std::uint8_t a = state.trns.present && raw == state.trns.key[0] ? 0 : 255;
          put(i, v, v, v, a);
        }
      }
      break;
    }
    case 2: {  // RGB
      if (depth == 16) {
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint16_t r = sample16(i * 3), g = sample16(i * 3 + 1), b = sample16(i * 3 + 2);
          const bool clear = state.trns.present && r == state.trns.key[0] && g == state.trns.key[1] && b == state.trns.key[2];
          put(i, r, g, b, clear ? 0 : 65535);
        }
      } else if (!state.trns.present && dx == 1 && x0 == 0) {
        for (std::uint32_t i = 0; i < count; ++i) {
          dst8[i * 4] = row[i * 3];
          dst8[i * 4 + 1] = row[i * 3 + 1];
          dst8[i * 4 + 2] = row[i * 3 + 2];
          dst8[i * 4 + 3] = 255;
        }
      } else {
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint8_t r = row[i * 3], g = row[i * 3 + 1], b = row[i * 3 + 2];
          const bool clear = state.trns.present && r == state.trns.key[0] && g == state.trns.key[1] && b == state.trns.key[2];
          put(i, r, g, b, clear ? 0 : 255);
        }
      }
      break;
    }
    case 3: {  // palette
      const int per_byte = 8 / depth;
      const std::uint32_t mask = (1u << depth) - 1u;
      const std::size_t entries = state.palette.entries.size();
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t index =
            depth < 8 ? (row[i / per_byte] >> (8 - depth - static_cast<int>(i % per_byte) * depth)) & mask : row[i];
        // Out-of-range indices are an error per the spec; like most decoders we show them as opaque black.
        const Rgba c = index < entries ? state.palette.entries[index] : Rgba{0, 0, 0, 255};
        put(i, c[0], c[1], c[2], c[3]);
      }
      break;
    }
    case 4: {  // grey + alpha
      if (depth == 16) {
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint16_t v = sample16(i * 2);
          put(i, v, v, v, sample16(i * 2 + 1));
        }
      } else {
        for (std::uint32_t i = 0; i < count; ++i) put(i, row[i * 2], row[i * 2], row[i * 2], row[i * 2 + 1]);
      }
      break;
    }
    default: {  // RGBA
      if (depth == 16) {
        for (std::uint32_t i = 0; i < count; ++i) {
          put(i, sample16(i * 4), sample16(i * 4 + 1), sample16(i * 4 + 2), sample16(i * 4 + 3));
        }
      } else if (dx == 1 && x0 == 0) {
        std::memcpy(dst8, row, static_cast<std::size_t>(count) * 4);
      } else {
        for (std::uint32_t i = 0; i < count; ++i) put(i, row[i * 4], row[i * 4 + 1], row[i * 4 + 2], row[i * 4 + 3]);
      }
      break;
    }
  }
}

/// Inflate and unfilter the image data of one picture of `width` x `height`.
Result<Pixels> decode_pixels(const DecodeState& state, std::uint32_t width, std::uint32_t height, ByteView zdata,
                             const Context& ctx, std::uint64_t& decompressed_total) {
  Header h = state.header;
  h.width = width;
  h.height = height;
  const std::uint64_t expected = raw_size(h);
  CVT_TRY(ctx.limits.check_decompressed(decompressed_total + expected));
  // DEFLATE cannot expand data by more than about 1032:1.
  CVT_TRY(check_plausible(expected, zdata.size(), 1100));
  auto raw = util::zlib_decompress(zdata, expected);
  Bytes data;
  if (!raw) {
    if (raw.error().code == Status::Limit) {
      // More data than the picture needs: decode again without the cap is
      // pointless; the extra bytes are ignored by other decoders too, so
      // take exactly what is needed via the incremental inflater.
      util::Inflater inflater(util::DeflateFormat::Zlib);
      Bytes out;
      const auto progress = inflater.feed(zdata, out, expected);
      (void)progress;
      if (out.size() < expected) return fail(Status::Decode, "PNG image data is corrupt");
      data = std::move(out);
    } else {
      // Truncated or corrupt stream: salvage what inflated, like libpng's
      // "incomplete image" handling, but only when most rows are present.
      util::Inflater inflater(util::DeflateFormat::Zlib);
      Bytes out;
      (void)inflater.feed(zdata, out, expected);
      if (out.empty()) return fail(Status::Decode, "PNG image data is corrupt: {}", raw.error().message);
      ctx.warn("The PNG image data is damaged or incomplete; missing rows were left transparent.");
      out.resize(expected, std::byte{0});
      data = std::move(out);
    }
  } else {
    data = std::move(*raw);
    if (data.size() < expected) {
      ctx.warn("The PNG image data is incomplete; missing rows were left transparent.");
      data.resize(expected, std::byte{0});
    }
  }
  decompressed_total += expected;

  Pixels out;
  out.width = width;
  out.height = height;
  out.rgba8.assign(static_cast<std::size_t>(width) * height * 4, 0);
  if (state.want16 && h.bit_depth == 16) out.rgba16.assign(out.rgba8.size(), 0);
  std::uint8_t* raw_bytes = reinterpret_cast<std::uint8_t*>(data.data());
  const std::size_t bpp = h.filter_bpp();
  std::size_t position = 0;
  const auto run_pass = [&](std::uint32_t x0, std::uint32_t y0, std::uint32_t dx, std::uint32_t dy) -> Result<void> {
    const std::uint32_t pw = pass_extent(width, x0, dx);
    const std::uint32_t ph = pass_extent(height, y0, dy);
    if (pw == 0 || ph == 0) return {};
    const auto row_len = static_cast<std::size_t>(h.row_bytes(pw));
    std::vector<std::uint8_t> zero(row_len, 0);
    const std::uint8_t* prior = zero.data();
    for (std::uint32_t r = 0; r < ph; ++r) {
      if ((r & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(position) / static_cast<double>(expected)));
      std::uint8_t* row = raw_bytes + position;
      const int filter = row[0];
      ++row;
      CVT_TRY(unfilter_row(filter, row, prior, row_len, bpp));
      expand_row(state, row, pw, x0, dx, y0 + r * dy, out);
      prior = row;
      position += row_len + 1;
    }
    return {};
  };
  if (h.interlaced) {
    for (const Pass& p : adam7) CVT_TRY(run_pass(p.x0, p.y0, p.dx, p.dy));
  } else {
    CVT_TRY(run_pass(0, 0, 1, 1));
  }
  return out;
}

struct FrameControl {
  std::uint32_t width = 0, height = 0, x = 0, y = 0;
  std::uint16_t delay_num = 0, delay_den = 0;
  int dispose = 0;
  int blend = 0;
  Bytes data;  // concatenated fdAT payloads (or IDAT for the default image)
};

std::string latin1_text(ByteView data) { return util::decode_text(data, util::TextEncoding::Latin1).text; }

/// Composite APNG frames onto a full canvas.
Result<std::vector<ir::Frame>> composite(const DecodeState& state, std::vector<FrameControl>& controls,
                                         const Context& ctx, std::uint64_t& decompressed_total) {
  const Header& h = state.header;
  CVT_TRY(check_size(ctx, h.width, h.height, controls.size()));
  std::uint64_t compressed = 0;
  for (const FrameControl& fc : controls) compressed += fc.data.size() + 64;
  CVT_TRY(check_plausible(std::uint64_t{h.width} * h.height * controls.size(), compressed, 1100 * 8));
  std::vector<ir::Frame> frames;
  std::vector<std::uint8_t> canvas(static_cast<std::size_t>(h.width) * h.height * 4, 0);
  std::vector<std::uint8_t> saved;
  DecodeState frame_state = state;
  frame_state.want16 = false;
  for (std::size_t n = 0; n < controls.size(); ++n) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(n) / controls.size(), "Decoding animation"));
    FrameControl& fc = controls[n];
    if (fc.width == 0 || fc.height == 0 || fc.x > h.width || fc.y > h.height || fc.width > h.width - fc.x ||
        fc.height > h.height - fc.y) {
      return fail(Status::Decode, "APNG frame {} lies outside the canvas", n);
    }
    int dispose = fc.dispose;
    if (n == 0 && dispose == 2) dispose = 1;
    if (dispose == 2) saved = canvas;
    CVT_TRY_ASSIGN(const Pixels region, decode_pixels(frame_state, fc.width, fc.height, fc.data, ctx, decompressed_total));
    for (std::uint32_t y = 0; y < fc.height; ++y) {
      std::uint8_t* dst = canvas.data() + (static_cast<std::size_t>(fc.y + y) * h.width + fc.x) * 4;
      const std::uint8_t* src = region.rgba8.data() + static_cast<std::size_t>(y) * fc.width * 4;
      if (fc.blend == 0) {
        std::memcpy(dst, src, static_cast<std::size_t>(fc.width) * 4);
        continue;
      }
      for (std::uint32_t x = 0; x < fc.width; ++x, dst += 4, src += 4) {
        const std::uint32_t sa = src[3];
        if (sa == 255) {
          std::memcpy(dst, src, 4);
        } else if (sa != 0) {
          const std::uint32_t da = dst[3];
          const std::uint32_t out_a = sa * 255 + da * (255 - sa);  // scaled by 255
          for (int c = 0; c < 3; ++c) {
            const std::uint32_t num = src[c] * sa * 255 + dst[c] * da * (255 - sa);
            dst[c] = static_cast<std::uint8_t>(out_a == 0 ? 0 : (num + out_a / 2) / out_a);
          }
          dst[3] = static_cast<std::uint8_t>((out_a + 127) / 255);
        }
      }
    }
    ir::Frame frame;
    frame.width = h.width;
    frame.height = h.height;
    frame.rgba = canvas;
    const std::uint32_t den = fc.delay_den == 0 ? 100 : fc.delay_den;
    frame.delay_ms = static_cast<std::uint32_t>((static_cast<std::uint64_t>(fc.delay_num) * 1000 + den / 2) / den);
    frames.push_back(std::move(frame));
    if (dispose == 1) {
      for (std::uint32_t y = 0; y < fc.height; ++y) {
        std::memset(canvas.data() + (static_cast<std::size_t>(fc.y + y) * h.width + fc.x) * 4, 0,
                    static_cast<std::size_t>(fc.width) * 4);
      }
    } else if (dispose == 2) {
      canvas = saved;
    }
  }
  return frames;
}

/// Convert gamma-encoded samples to sRGB: v_linear = v^(1/gamma).
void apply_gamma(ir::Image& image, double file_gamma) {
  if (!(file_gamma > 0.0) || std::abs(file_gamma - 1.0 / 2.2) < 0.01) return;
  std::array<std::uint8_t, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    lut[static_cast<std::size_t>(i)] = linear_to_srgb8(static_cast<float>(std::pow(i / 255.0, 1.0 / file_gamma)));
  }
  for (ir::Frame& frame : image.frames) {
    for (std::size_t i = 0; i + 3 < frame.rgba.size(); i += 4) {
      frame.rgba[i] = lut[frame.rgba[i]];
      frame.rgba[i + 1] = lut[frame.rgba[i + 1]];
      frame.rgba[i + 2] = lut[frame.rgba[i + 2]];
    }
  }
}

}  // namespace

Result<ir::Image> decode(ByteView data, const Context& ctx) {
  util::ByteReader reader(data);
  if (!reader.consume(signature)) return fail(Status::Decode, "not a PNG file");
  DecodeState state;
  state.want16 = true;
  bool have_header = false;
  bool have_end = false;
  bool idat_seen = false;
  bool idat_done = false;
  Bytes idat;
  ir::Image image;
  double gamma = 0.0;
  bool srgb_or_icc = false;
  // APNG
  bool animated = false;
  std::uint32_t declared_frames = 0;
  std::vector<FrameControl> controls;
  bool default_is_frame = false;
  std::optional<std::uint32_t> expected_sequence;

  while (!reader.at_end()) {
    const std::uint32_t length = reader.u32be();
    const std::string_view type = reader.chars(4);
    if (!reader.ok()) break;
    if (length > 0x7FFFFFFFu || length > reader.remaining()) {
      if (have_header && (idat_seen || !controls.empty())) {
        ctx.warn("The PNG file is truncated.");
        break;
      }
      return fail(Status::Decode, "PNG chunk '{}' is truncated", util::sanitize_utf8(type));
    }
    const ByteView body = reader.bytes(length);
    const std::uint32_t stored_crc = reader.u32be();
    const bool crc_ok = reader.ok() && util::crc32(body, util::crc32(as_bytes(type))) == stored_crc;
    const bool critical = (type[0] & 0x20) == 0;
    if (!crc_ok) {
      if (critical && type != "IEND") {
        if (type == "IDAT" && idat_seen) {
          ctx.warn("A PNG data chunk failed its checksum; the picture may be damaged.");
        } else {
          return fail(Status::Decode, "PNG chunk '{}' is damaged (checksum mismatch)", util::sanitize_utf8(type));
        }
      } else {
        ctx.warn(std::format("Skipped the damaged PNG chunk '{}'.", util::sanitize_utf8(type)));
        if (type == "IEND") have_end = true;
        if (type != "IEND") continue;
        break;
      }
    }
    util::ByteReader chunk(body);
    if (!have_header) {
      if (type != "IHDR" || length != 13) return fail(Status::Decode, "PNG file does not start with a valid IHDR chunk");
      Header& h = state.header;
      h.width = chunk.u32be();
      h.height = chunk.u32be();
      h.bit_depth = chunk.u8();
      h.color_type = chunk.u8();
      const int compression = chunk.u8();
      const int filter = chunk.u8();
      const int interlace = chunk.u8();
      if (compression != 0 || filter != 0 || interlace > 1) return fail(Status::Decode, "PNG header uses unknown methods");
      const int d = h.bit_depth;
      bool valid = false;
      switch (h.color_type) {
        case 0: valid = d == 1 || d == 2 || d == 4 || d == 8 || d == 16; break;
        case 3: valid = d == 1 || d == 2 || d == 4 || d == 8; break;
        case 2:
        case 4:
        case 6: valid = d == 8 || d == 16; break;
        default: valid = false;
      }
      if (!valid) return fail(Status::Decode, "invalid PNG colour type {} with bit depth {}", h.color_type, d);
      h.interlaced = interlace == 1;
      if (h.width > 0x7FFFFFFFu || h.height > 0x7FFFFFFFu) return fail(Status::Decode, "PNG dimensions are out of range");
      CVT_TRY(check_size(ctx, h.width, h.height));
      have_header = true;
      continue;
    }
    if (type == "IEND") {
      have_end = true;
      break;
    }
    if (type == "IDAT") {
      if (idat_done) {
        ctx.warn("The PNG file has data chunks out of order; extra data was ignored.");
        continue;
      }
      idat_seen = true;
      CVT_TRY(ctx.limits.check_decompressed(idat.size() + body.size()));
      append(idat, body);
      continue;
    }
    if (idat_seen && !idat_done) idat_done = true;
    if (type == "PLTE") {
      if (length % 3 != 0 || length == 0 || length > 768) return fail(Status::Decode, "PNG palette has an invalid size");
      const std::size_t alpha_known = state.palette.entries.size();
      state.palette.entries.resize(length / 3, Rgba{0, 0, 0, 255});
      for (std::size_t i = 0; i < length / 3; ++i) {
        Rgba& e = state.palette.entries[i];
        e[0] = chunk.u8();
        e[1] = chunk.u8();
        e[2] = chunk.u8();
        if (i >= alpha_known) e[3] = 255;
      }
    } else if (type == "tRNS") {
      const int ct = state.header.color_type;
      if (ct == 3) {
        if (state.palette.entries.size() < length) state.palette.entries.resize(length, Rgba{0, 0, 0, 255});
        for (std::size_t i = 0; i < length; ++i) state.palette.entries[i][3] = chunk.u8();
      } else if (ct == 0 && length >= 2) {
        state.trns.present = true;
        state.trns.key[0] = chunk.u16be();
      } else if (ct == 2 && length >= 6) {
        state.trns.present = true;
        for (auto& k : state.trns.key) k = chunk.u16be();
      }
    } else if (type == "gAMA" && length == 4) {
      const std::uint32_t g = chunk.u32be();
      if (g > 0) gamma = g / 100000.0;
    } else if (type == "sRGB") {
      srgb_or_icc = true;
    } else if (type == "iCCP") {
      srgb_or_icc = true;
      const std::string_view rest = as_chars(body);
      const std::size_t nul = rest.find('\0');
      if (nul != std::string_view::npos && nul + 2 <= rest.size() && body[nul + 1] == std::byte{0}) {
        auto profile = util::zlib_decompress(body.subspan(nul + 2), 16u * 1024 * 1024);
        if (profile) image.icc_profile = std::move(*profile);
      }
    } else if (type == "pHYs" && length == 9) {
      const std::uint32_t px = chunk.u32be();
      const std::uint32_t py = chunk.u32be();
      if (chunk.u8() == 1 && px > 0 && py > 0) {
        image.dpi_x = px * 0.0254;
        image.dpi_y = py * 0.0254;
      }
    } else if (type == "tEXt") {
      const std::string_view text = as_chars(body);
      const std::size_t nul = text.find('\0');
      if (nul != std::string_view::npos && nul > 0) {
        image.metadata[latin1_text(body.first(nul))] = latin1_text(body.subspan(nul + 1));
      }
    } else if (type == "zTXt") {
      const std::string_view text = as_chars(body);
      const std::size_t nul = text.find('\0');
      if (nul != std::string_view::npos && nul > 0 && nul + 2 <= text.size()) {
        auto value = util::zlib_decompress(body.subspan(nul + 2), 4u * 1024 * 1024);
        if (value) image.metadata[latin1_text(body.first(nul))] = latin1_text(*value);
      }
    } else if (type == "iTXt") {
      // keyword\0 flag method language\0 translated\0 text
      const std::string_view text = as_chars(body);
      const std::size_t k = text.find('\0');
      if (k != std::string_view::npos && k > 0 && k + 3 <= text.size()) {
        const bool compressed = text[k + 1] != 0;
        const std::size_t lang_end = text.find('\0', k + 3);
        const std::size_t trans_end = lang_end == std::string_view::npos ? lang_end : text.find('\0', lang_end + 1);
        if (trans_end != std::string_view::npos) {
          std::string value;
          if (compressed) {
            auto inflated = util::zlib_decompress(body.subspan(trans_end + 1), 16u * 1024 * 1024);
            if (inflated) value = to_string(*inflated);
          } else {
            value = std::string(text.substr(trans_end + 1));
          }
          const std::string key = util::sanitize_utf8(text.substr(0, k));
          if (key == "XML:com.adobe.xmp") {
            image.xmp = util::sanitize_utf8(value);
          } else {
            image.metadata[key] = util::sanitize_utf8(value);
          }
        }
      }
    } else if (type == "eXIf") {
      image.exif = to_bytes(body);
    } else if (type == "acTL" && length == 8 && !idat_seen) {
      declared_frames = chunk.u32be();
      image.loop_count = chunk.u32be();
      animated = declared_frames > 0;
      if (declared_frames > ctx.limits.max_archive_entries) {
        return fail(Status::Limit, "APNG declares {} frames", declared_frames);
      }
    } else if (type == "fcTL" && length == 26) {
      const std::uint32_t sequence = chunk.u32be();
      if (expected_sequence && sequence != *expected_sequence) ctx.warn("APNG frame chunks are out of sequence.");
      expected_sequence = sequence + 1;
      FrameControl fc;
      fc.width = chunk.u32be();
      fc.height = chunk.u32be();
      fc.x = chunk.u32be();
      fc.y = chunk.u32be();
      fc.delay_num = chunk.u16be();
      fc.delay_den = chunk.u16be();
      fc.dispose = chunk.u8();
      fc.blend = chunk.u8();
      if (fc.dispose > 2 || fc.blend > 1) return fail(Status::Decode, "APNG frame control has invalid modes");
      if (!idat_seen) default_is_frame = true;
      if (controls.size() >= ctx.limits.max_archive_entries) return fail(Status::Limit, "too many APNG frames");
      controls.push_back(std::move(fc));
    } else if (type == "fdAT" && length >= 4) {
      const std::uint32_t sequence = chunk.u32be();
      if (expected_sequence && sequence != *expected_sequence) ctx.warn("APNG frame chunks are out of sequence.");
      expected_sequence = sequence + 1;
      if (controls.empty()) continue;
      FrameControl& fc = controls.back();
      CVT_TRY(ctx.limits.check_decompressed(fc.data.size() + body.size()));
      append(fc.data, body.subspan(4));
    } else if (critical) {
      return fail(Status::Decode, "PNG file uses the unknown critical chunk '{}'", util::sanitize_utf8(type));
    }
  }
  if (!have_header) return fail(Status::Decode, "PNG file has no header");
  if (!idat_seen) return fail(Status::Decode, "PNG file has no image data");
  if (!have_end) ctx.warn("The PNG file has no end marker; it may be truncated.");
  if (state.header.color_type == 3 && state.palette.entries.empty()) {
    return fail(Status::Decode, "PNG palette image has no palette");
  }

  std::uint64_t decompressed = 0;
  // Drop frames that carry no data (truncated animation).
  while (!controls.empty() && controls.back().data.empty() && !(default_is_frame && controls.size() == 1)) {
    controls.pop_back();
  }
  if (animated && !controls.empty() && (controls.size() > 1 || !default_is_frame)) {
    if (default_is_frame) controls.front().data = idat;
    if (controls.size() != declared_frames) ctx.warn("The APNG animation has fewer frames than it declares.");
    CVT_TRY_ASSIGN(image.frames, composite(state, controls, ctx, decompressed));
  } else {
    CVT_TRY_ASSIGN(Pixels pixels, decode_pixels(state, state.header.width, state.header.height, idat, ctx, decompressed));
    ir::Frame frame;
    frame.width = pixels.width;
    frame.height = pixels.height;
    frame.rgba = std::move(pixels.rgba8);
    image.frames.push_back(std::move(frame));
    if (!pixels.rgba16.empty()) {
      image.hdr = linear_from_srgb16(pixels.rgba16, pixels.width, pixels.height);
    }
  }
  image.source_bit_depth = static_cast<std::uint8_t>(state.header.bit_depth);
  image.source_grayscale = state.header.color_type == 0 || state.header.color_type == 4;
  if (!srgb_or_icc && gamma > 0.0) {
    apply_gamma(image, gamma);
    if (image.hdr && std::abs(gamma - 1.0 / 2.2) >= 0.01) {
      for (std::size_t i = 0; i + 3 < image.hdr->rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
          const float encoded = linear_to_srgb(image.hdr->rgba[i + c]);
          image.hdr->rgba[i + c] = static_cast<float>(std::pow(static_cast<double>(encoded), 1.0 / gamma));
        }
      }
    }
  }
  if (!image.exif.empty()) {
    const Bytes exif = image.exif;
    image.exif.clear();
    attach_exif(image, exif);
  }
  return image;
}

// --------------------------------------------------------------------------- encoding

namespace {

class ChunkWriter {
 public:
  void signature_bytes() { out_.text(signature); }
  void chunk(std::string_view type, ByteView body) {
    out_.u32be(static_cast<std::uint32_t>(body.size()));
    out_.text(type);
    out_.bytes(body);
    out_.u32be(util::crc32(body, util::crc32(as_bytes(type))));
  }
  [[nodiscard]] Bytes take() { return out_.take(); }
  [[nodiscard]] std::size_t size() const { return out_.size(); }

 private:
  util::ByteWriter out_;
};

/// How pixels are stored.
struct Plan {
  int color_type = 6;
  int bit_depth = 8;
  std::vector<Rgba> palette;        // colour type 3
  bool use_key = false;             // tRNS colour key (types 0 and 2)
  std::array<std::uint16_t, 3> key = {0, 0, 0};
  bool sixteen = false;             // samples come from 16-bit data
};

/// One picture to encode: either 8-bit RGBA, palette indices or 16-bit RGBA.
struct Source {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  const std::uint8_t* rgba8 = nullptr;     // stride width*4
  const std::uint8_t* indices = nullptr;   // stride width (palette plans)
  const std::uint16_t* rgba16 = nullptr;   // stride width*4
  std::uint32_t stride_px = 0;             // row stride in pixels of the source buffers
  std::uint32_t x0 = 0, y0 = 0;            // sub-rectangle origin
};

/// Pack row `y` (pass pixels x0 + i*dx) of `src` into PNG sample bytes.
void pack_row(const Plan& plan, const Source& src, std::uint32_t y, std::uint32_t x0, std::uint32_t dx,
              std::uint32_t count, std::uint8_t* out) {
  const std::size_t base = static_cast<std::size_t>(src.y0 + y) * src.stride_px + src.x0;
  const int depth = plan.bit_depth;
  if (plan.color_type == 3) {
    const std::uint8_t* idx = src.indices + base;
    if (depth == 8) {
      for (std::uint32_t i = 0; i < count; ++i) out[i] = idx[x0 + i * dx];
      return;
    }
    const int per_byte = 8 / depth;
    std::memset(out, 0, (count + static_cast<std::uint32_t>(per_byte) - 1) / static_cast<std::uint32_t>(per_byte));
    for (std::uint32_t i = 0; i < count; ++i) {
      out[i / static_cast<std::uint32_t>(per_byte)] |= static_cast<std::uint8_t>(
          idx[x0 + i * dx] << (8 - depth - static_cast<int>(i % static_cast<std::uint32_t>(per_byte)) * depth));
    }
    return;
  }
  if (plan.sixteen) {
    const std::uint16_t* p = src.rgba16 + base * 4;
    std::size_t o = 0;
    const auto put = [&](std::uint16_t v) {
      out[o++] = static_cast<std::uint8_t>(v >> 8);
      out[o++] = static_cast<std::uint8_t>(v);
    };
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint16_t* q = p + static_cast<std::size_t>(x0 + i * dx) * 4;
      switch (plan.color_type) {
        case 0: put(q[3] == 0 && plan.use_key ? plan.key[0] : q[0]); break;
        case 4: put(q[0]); put(q[3]); break;
        case 2:
          if (q[3] == 0 && plan.use_key) {
            put(plan.key[0]); put(plan.key[1]); put(plan.key[2]);
          } else {
            put(q[0]); put(q[1]); put(q[2]);
          }
          break;
        default: put(q[0]); put(q[1]); put(q[2]); put(q[3]); break;
      }
    }
    return;
  }
  const std::uint8_t* p = src.rgba8 + base * 4;
  switch (plan.color_type) {
    case 0: {
      if (depth == 8) {
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint8_t* q = p + static_cast<std::size_t>(x0 + i * dx) * 4;
          out[i] = q[3] == 0 && plan.use_key ? static_cast<std::uint8_t>(plan.key[0]) : q[0];
        }
        return;
      }
      const int per_byte = 8 / depth;
      std::memset(out, 0, (count + static_cast<std::uint32_t>(per_byte) - 1) / static_cast<std::uint32_t>(per_byte));
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* q = p + static_cast<std::size_t>(x0 + i * dx) * 4;
        const std::uint32_t level = q[3] == 0 && plan.use_key ? plan.key[0] : static_cast<std::uint32_t>(q[0] >> (8 - depth));
        out[i / static_cast<std::uint32_t>(per_byte)] |= static_cast<std::uint8_t>(
            level << (8 - depth - static_cast<int>(i % static_cast<std::uint32_t>(per_byte)) * depth));
      }
      return;
    }
    case 4:
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* q = p + static_cast<std::size_t>(x0 + i * dx) * 4;
        out[i * 2] = q[0];
        out[i * 2 + 1] = q[3];
      }
      return;
    case 2:
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* q = p + static_cast<std::size_t>(x0 + i * dx) * 4;
        if (q[3] == 0 && plan.use_key) {
          out[i * 3] = static_cast<std::uint8_t>(plan.key[0]);
          out[i * 3 + 1] = static_cast<std::uint8_t>(plan.key[1]);
          out[i * 3 + 2] = static_cast<std::uint8_t>(plan.key[2]);
        } else {
          out[i * 3] = q[0];
          out[i * 3 + 1] = q[1];
          out[i * 3 + 2] = q[2];
        }
      }
      return;
    default:
      if (dx == 1) {
        std::memcpy(out, p + static_cast<std::size_t>(x0) * 4, static_cast<std::size_t>(count) * 4);
        return;
      }
      for (std::uint32_t i = 0; i < count; ++i) std::memcpy(out + i * 4, p + static_cast<std::size_t>(x0 + i * dx) * 4, 4);
      return;
  }
}

enum class FilterStrategy { None, Adaptive };

/// Filter one packed row into `out` (1 + length bytes) and return the filter used.
void filter_row(FilterStrategy strategy, const std::uint8_t* row, const std::uint8_t* prior, std::size_t length,
                std::size_t bpp, std::uint8_t* out, std::vector<std::uint8_t>& scratch) {
  if (strategy == FilterStrategy::None) {
    out[0] = 0;
    std::memcpy(out + 1, row, length);
    return;
  }
  scratch.resize(length * 5);
  std::uint8_t* f[5] = {scratch.data(), scratch.data() + length, scratch.data() + 2 * length,
                        scratch.data() + 3 * length, scratch.data() + 4 * length};
  std::uint64_t cost[5] = {0, 0, 0, 0, 0};
  for (std::size_t i = 0; i < length; ++i) {
    const int a = i >= bpp ? row[i - bpp] : 0;
    const int b = prior[i];
    const int c = i >= bpp ? prior[i - bpp] : 0;
    const int x = row[i];
    const std::uint8_t v[5] = {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x - a),
                               static_cast<std::uint8_t>(x - b), static_cast<std::uint8_t>(x - ((a + b) >> 1)),
                               static_cast<std::uint8_t>(x - paeth(a, b, c))};
    for (int k = 0; k < 5; ++k) {
      f[k][i] = v[k];
      cost[k] += static_cast<std::uint64_t>(std::abs(static_cast<int>(static_cast<std::int8_t>(v[k]))));
    }
  }
  int best = 0;
  for (int k = 1; k < 5; ++k) {
    if (cost[k] < cost[best]) best = k;
  }
  out[0] = static_cast<std::uint8_t>(best);
  std::memcpy(out + 1, f[best], length);
}

Result<Bytes> filtered_data(const Plan& plan, const Source& src, bool interlace, FilterStrategy strategy,
                            const Context& ctx) {
  Header h;
  h.width = src.width;
  h.height = src.height;
  h.bit_depth = plan.bit_depth;
  h.color_type = plan.color_type;
  h.interlaced = interlace;
  const std::uint64_t total = raw_size(h);
  if (total > static_cast<std::uint64_t>(SIZE_MAX / 2)) return fail(Status::Limit, "PNG image is too large");
  Bytes data(static_cast<std::size_t>(total));
  auto* out = reinterpret_cast<std::uint8_t*>(data.data());
  const std::size_t bpp = h.filter_bpp();
  std::vector<std::uint8_t> scratch;
  std::size_t position = 0;
  const auto run_pass = [&](std::uint32_t x0, std::uint32_t y0, std::uint32_t dx, std::uint32_t dy) -> Result<void> {
    const std::uint32_t pw = pass_extent(src.width, x0, dx);
    const std::uint32_t ph = pass_extent(src.height, y0, dy);
    if (pw == 0 || ph == 0) return {};
    const auto length = static_cast<std::size_t>(h.row_bytes(pw));
    std::vector<std::uint8_t> rows[2] = {std::vector<std::uint8_t>(length, 0), std::vector<std::uint8_t>(length, 0)};
    for (std::uint32_t r = 0; r < ph; ++r) {
      if ((r & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(position) / static_cast<double>(total)));
      std::vector<std::uint8_t>& current = rows[r & 1];
      const std::vector<std::uint8_t>& prior = rows[(r + 1) & 1];
      pack_row(plan, src, y0 + r * dy, x0, dx, pw, current.data());
      filter_row(strategy, current.data(), prior.data(), length, bpp, out + position, scratch);
      position += length + 1;
    }
    return {};
  };
  if (interlace) {
    for (const Pass& p : adam7) CVT_TRY(run_pass(p.x0, p.y0, p.dx, p.dy));
  } else {
    CVT_TRY(run_pass(0, 0, 1, 1));
  }
  return data;
}

/// Compress the image data of one picture, trying the strategies the
/// effort level allows and keeping the smallest.
Result<Bytes> compressed_data(const Plan& plan, const Source& src, bool interlace, int level, const Context& ctx) {
  const bool low_depth = plan.color_type == 3 || plan.bit_depth < 8;
  std::vector<FilterStrategy> strategies;
  if (level == 0) {
    strategies = {FilterStrategy::None};
  } else if (level >= 8) {
    strategies = {FilterStrategy::Adaptive, FilterStrategy::None};
  } else {
    strategies = {low_depth ? FilterStrategy::None : FilterStrategy::Adaptive};
  }
  std::optional<Bytes> best;
  for (const FilterStrategy strategy : strategies) {
    CVT_TRY_ASSIGN(const Bytes raw, filtered_data(plan, src, interlace, strategy, ctx));
    CVT_TRY_ASSIGN(Bytes z, util::zlib_compress(raw, level));
    if (!best || z.size() < best->size()) best = std::move(z);
  }
  return std::move(*best);
}

Bytes header_chunk(std::uint32_t width, std::uint32_t height, const Plan& plan, bool interlace) {
  util::ByteWriter w;
  w.u32be(width).u32be(height).u8(static_cast<std::uint8_t>(plan.bit_depth)).u8(static_cast<std::uint8_t>(plan.color_type));
  w.u8(0).u8(0).u8(interlace ? 1 : 0);
  return w.take();
}

int depth_for_count(std::size_t colors) {
  if (colors <= 2) return 1;
  if (colors <= 4) return 2;
  if (colors <= 16) return 4;
  return 8;
}

/// A grey level (at `depth` bits) that no opaque pixel uses, for a tRNS key.
std::optional<std::uint16_t> free_grey_key(std::span<const ir::Frame* const> frames, int depth) {
  const int levels = 1 << depth;
  std::vector<char> used(static_cast<std::size_t>(levels), 0);
  for (const ir::Frame* f : frames) {
    for (std::size_t i = 0; i + 3 < f->rgba.size(); i += 4) {
      if (f->rgba[i + 3] != 0) used[static_cast<std::size_t>(f->rgba[i] >> (8 - depth))] = 1;
    }
  }
  for (int v = 0; v < levels; ++v) {
    if (!used[static_cast<std::size_t>(v)]) return static_cast<std::uint16_t>(v);
  }
  return std::nullopt;
}

std::optional<std::array<std::uint16_t, 3>> free_rgb_key(std::span<const ir::Frame* const> frames) {
  // Try a few candidates; a colour absent from every opaque pixel is enough.
  std::vector<std::uint32_t> colors;
  for (const ir::Frame* f : frames) {
    for (std::size_t i = 0; i + 3 < f->rgba.size(); i += 4) {
      if (f->rgba[i + 3] != 0) {
        colors.push_back(static_cast<std::uint32_t>(f->rgba[i]) << 16 | static_cast<std::uint32_t>(f->rgba[i + 1]) << 8 |
                         f->rgba[i + 2]);
      }
    }
  }
  std::sort(colors.begin(), colors.end());
  colors.erase(std::unique(colors.begin(), colors.end()), colors.end());
  for (std::uint32_t candidate : {0x000000u, 0xFFFFFFu, 0xFF00FFu, 0x00FF00u, 0x010203u}) {
    if (!std::binary_search(colors.begin(), colors.end(), candidate)) {
      return std::array<std::uint16_t, 3>{static_cast<std::uint16_t>(candidate >> 16),
                                          static_cast<std::uint16_t>((candidate >> 8) & 0xFF),
                                          static_cast<std::uint16_t>(candidate & 0xFF)};
    }
  }
  std::uint32_t candidate = 0;
  for (const std::uint32_t c : colors) {
    if (c != candidate) break;
    ++candidate;
  }
  if (candidate > 0xFFFFFFu) return std::nullopt;
  return std::array<std::uint16_t, 3>{static_cast<std::uint16_t>(candidate >> 16),
                                      static_cast<std::uint16_t>((candidate >> 8) & 0xFF),
                                      static_cast<std::uint16_t>(candidate & 0xFF)};
}

bool frames_binary_alpha(std::span<const ir::Frame* const> frames) {
  for (const ir::Frame* f : frames) {
    if (!has_binary_alpha(*f)) return false;
  }
  return true;
}

/// Choose a lossless 8-bit storage for the frames.
Plan plan_lossless(std::span<const ir::Frame* const> frames, std::optional<Quantized>& exact) {
  bool opaque = true;
  bool grey = true;
  for (const ir::Frame* f : frames) {
    opaque = opaque && is_opaque(*f);
    grey = grey && is_grayscale(*f);
  }
  Plan plan;
  // Palette candidate (also covers transparency through tRNS).
  exact.reset();
  if (frames.size() == 1) {
    exact = exact_palette(*frames[0], 256);
  } else {
    // A shared palette for all frames of an animation: stack them.
    ir::Frame stacked;
    stacked.width = frames[0]->width;
    stacked.height = static_cast<std::uint32_t>(frames[0]->height * frames.size());
    std::uint64_t total = std::uint64_t{frames[0]->width} * frames[0]->height * frames.size();
    if (total <= (1u << 26)) {
      for (const ir::Frame* f : frames) stacked.rgba.insert(stacked.rgba.end(), f->rgba.begin(), f->rgba.end());
      exact = exact_palette(stacked, 256);
    }
  }
  if (grey) {
    const int depth = std::max(1, [&] {
      int d = 1;
      for (const ir::Frame* f : frames) d = std::max(d, lossless_bit_depth(*f, false));
      return d;
    }());
    if (opaque) {
      plan.color_type = 0;
      plan.bit_depth = depth;
      // A palette is only better when it allows a lower depth.
      if (exact && depth_for_count(exact->palette.size()) < depth) {
        plan.color_type = 3;
        plan.bit_depth = depth_for_count(exact->palette.size());
        plan.palette = exact->palette;
      }
      return plan;
    }
    if (frames_binary_alpha(frames)) {
      for (int d = depth; d <= 8; d *= 2) {
        if (auto key = free_grey_key(frames, d)) {
          plan.color_type = 0;
          plan.bit_depth = d;
          plan.use_key = true;
          plan.key = {*key, 0, 0};
          if (exact && depth_for_count(exact->palette.size()) < d) break;
          return plan;
        }
      }
    }
  }
  if (exact) {
    plan.color_type = 3;
    plan.bit_depth = depth_for_count(exact->palette.size());
    plan.palette = exact->palette;
    return plan;
  }
  if (grey) {
    plan.color_type = 4;
    plan.bit_depth = 8;
    return plan;
  }
  if (opaque) {
    plan.color_type = 2;
    return plan;
  }
  if (frames_binary_alpha(frames)) {
    if (auto key = free_rgb_key(frames)) {
      plan.color_type = 2;
      plan.use_key = true;
      plan.key = *key;
      return plan;
    }
  }
  plan.color_type = 6;
  return plan;
}

/// Plan for 16-bit samples.
Plan plan_sixteen(const std::vector<std::uint16_t>& rgba16) {
  bool grey = true;
  bool opaque = true;
  for (std::size_t i = 0; i + 3 < rgba16.size(); i += 4) {
    if (rgba16[i] != rgba16[i + 1] || rgba16[i] != rgba16[i + 2]) grey = false;
    if (rgba16[i + 3] != 65535) opaque = false;
  }
  Plan plan;
  plan.bit_depth = 16;
  plan.sixteen = true;
  plan.color_type = grey ? (opaque ? 0 : 4) : (opaque ? 2 : 6);
  return plan;
}

void write_palette(ChunkWriter& out, const Plan& plan) {
  Bytes plte;
  Bytes trns;
  std::size_t last_alpha = 0;
  for (std::size_t i = 0; i < plan.palette.size(); ++i) {
    if (plan.palette[i][3] != 255) last_alpha = i + 1;
  }
  for (std::size_t i = 0; i < plan.palette.size(); ++i) {
    plte.push_back(std::byte{plan.palette[i][0]});
    plte.push_back(std::byte{plan.palette[i][1]});
    plte.push_back(std::byte{plan.palette[i][2]});
    if (i < last_alpha) trns.push_back(std::byte{plan.palette[i][3]});
  }
  out.chunk("PLTE", plte);
  if (!trns.empty()) out.chunk("tRNS", trns);
}

void write_key(ChunkWriter& out, const Plan& plan) {
  if (!plan.use_key) return;
  util::ByteWriter w;
  if (plan.color_type == 0) {
    w.u16be(plan.key[0]);
  } else {
    w.u16be(plan.key[0]).u16be(plan.key[1]).u16be(plan.key[2]);
  }
  out.chunk("tRNS", w.view());
}

void write_metadata(ChunkWriter& out, const ir::Image& image, bool keep) {
  if (image.dpi_x > 0 && image.dpi_y > 0) {
    util::ByteWriter w;
    w.u32be(static_cast<std::uint32_t>(std::lround(image.dpi_x / 0.0254)));
    w.u32be(static_cast<std::uint32_t>(std::lround(image.dpi_y / 0.0254)));
    w.u8(1);
    out.chunk("pHYs", w.view());
  }
  if (!keep) return;
  if (!image.icc_profile.empty()) {
    auto z = util::zlib_compress(image.icc_profile, 9);
    if (z) {
      Bytes body = to_bytes("ICC profile"sv);
      body.push_back(std::byte{0});
      body.push_back(std::byte{0});
      append(body, *z);
      out.chunk("iCCP", body);
    }
  }
  if (!image.exif.empty()) out.chunk("eXIf", image.exif);
  const auto itxt = [&](std::string_view key, std::string_view value) {
    Bytes body = to_bytes(key.substr(0, 79));
    for (int i = 0; i < 3; ++i) body.push_back(std::byte{0});  // NUL, uncompressed, method
    body.push_back(std::byte{0});                              // empty language
    body.push_back(std::byte{0});                              // empty translated keyword
    append(body, value);
    out.chunk("iTXt", body);
  };
  if (!image.xmp.empty()) itxt("XML:com.adobe.xmp", image.xmp);
  for (const auto& [key, value] : image.metadata) {
    if (key.empty() || key.size() > 79) continue;
    itxt(key, value);
  }
}

/// Bounding box of pixels that differ between two equally sized frames.
struct Rect {
  std::uint32_t x = 0, y = 0, w = 0, h = 0;
};
Rect changed_rect(const ir::Frame& a, const ir::Frame& b) {
  std::uint32_t x0 = a.width, y0 = a.height, x1 = 0, y1 = 0;
  bool any = false;
  for (std::uint32_t y = 0; y < a.height; ++y) {
    const std::uint8_t* pa = a.rgba.data() + static_cast<std::size_t>(y) * a.width * 4;
    const std::uint8_t* pb = b.rgba.data() + static_cast<std::size_t>(y) * a.width * 4;
    if (std::memcmp(pa, pb, static_cast<std::size_t>(a.width) * 4) == 0) continue;
    for (std::uint32_t x = 0; x < a.width; ++x) {
      if (std::memcmp(pa + x * 4, pb + x * 4, 4) != 0) {
        any = true;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
      }
    }
  }
  if (!any) return {0, 0, 1, 1};
  return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

Result<Bytes> encode_impl(const ir::Image& image, const EncodeOptions& options, const Context& ctx, bool& used_palette);

}  // namespace

Result<Bytes> encode(const ir::Image& image, const EncodeOptions& options, const Context& ctx) {
  bool used_palette = false;
  CVT_TRY_ASSIGN(Bytes first, encode_impl(image, options, ctx, used_palette));
  if (options.lossy_quality > 0 && used_palette) {
    // The palette is not always smaller (smooth gradients filter very well):
    // keep whichever encoding is smaller.
    EncodeOptions lossless = options;
    lossless.lossy_quality = 0;
    bool unused = false;
    CVT_TRY_ASSIGN(Bytes second, encode_impl(image, lossless, ctx, unused));
    if (second.size() <= first.size()) return second;
  }
  return first;
}

namespace {

Result<Bytes> encode_impl(const ir::Image& image, const EncodeOptions& options, const Context& ctx, bool& used_palette) {
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  const std::uint32_t width = image.width();
  const std::uint32_t height = image.height();
  for (const ir::Frame& f : image.frames) {
    if (f.width != width || f.height != height) return fail(Status::Encode, "animation frames differ in size");
  }
  const int level = std::clamp(options.compression_level, 0, 9);
  const bool animated = image.animated();
  std::vector<const ir::Frame*> frames;
  for (const ir::Frame& f : image.frames) frames.push_back(&f);

  // Decide the storage plan.
  Plan plan;
  std::vector<Quantized> mapped;  // per-frame palette indices (palette plans)
  std::vector<std::uint16_t> wide;
  const bool can16 = !animated && options.lossy_quality == 0 && image.hdr && image.hdr->width == width &&
                     image.hdr->height == height && options.bit_depth != 8 &&
                     (options.bit_depth == 16 || image.source_bit_depth > 8);
  if (can16) {
    // Keep 16 bits only when the HDR data fits the 0..1 range PNG can hold.
    bool in_range = true;
    for (const float v : image.hdr->rgba) {
      if (!(v >= 0.0f && v <= 1.0001f)) {
        in_range = false;
        break;
      }
    }
    if (in_range || options.bit_depth == 16) {
      wide = srgb16_from_linear(*image.hdr);
      plan = plan_sixteen(wide);
    }
  }
  if (!plan.sixteen) {
    std::optional<Quantized> exact;
    if (options.bit_depth == 16) {
      // Expand 8-bit data to 16 bits on request.
      if (animated) return fail(Status::Encode, "16-bit output is not available for animations");
      wide.resize(image.frames[0].rgba.size());
      for (std::size_t i = 0; i < wide.size(); ++i) wide[i] = static_cast<std::uint16_t>(image.frames[0].rgba[i] * 257u);
      plan = plan_sixteen(wide);
    } else {
      plan = plan_lossless(frames, exact);
      const bool lossless_palette = plan.color_type == 3;
      if (options.lossy_quality > 0 && !lossless_palette && plan.color_type != 0) {
        // The "small" path: quantise to a palette whose size follows the quality.
        const int q = options.lossy_quality;
        QuantizeOptions qo;
        qo.max_colors = q >= 70 ? 256 : q >= 50 ? 128 : q >= 30 ? 64 : q >= 10 ? 32 : 16;
        qo.dither = options.dither == 0 ? Dither::None : options.dither == 2 ? Dither::Ordered : Dither::FloydSteinberg;
        qo.dither_strength = 1.0f;
        qo.refine_iterations = q >= 50 ? 6 : 3;
        const std::vector<Rgba> palette = build_palette(frames, qo);
        std::vector<Quantized> candidate;
        double worst_mse = 0.0;
        for (const ir::Frame* f : frames) {
          CVT_TRY_ASSIGN(Quantized qz, remap(*f, palette, qo));
          worst_mse = std::max(worst_mse, qz.mse);
          candidate.push_back(std::move(qz));
        }
        // Keep the palette only when it meets the quality's PSNR floor;
        // otherwise the lossless encoding is the honest answer.
        const double floor_db = 15.0 + 0.2 * q;
        const double psnr = worst_mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / worst_mse);
        if (psnr >= floor_db) {
          plan = Plan{};
          plan.color_type = 3;
          plan.palette = palette;
          plan.bit_depth = depth_for_count(palette.size());
          mapped = std::move(candidate);
          used_palette = true;
        }
      }
      if (plan.color_type == 3 && mapped.empty()) {
        // Lossless palette: map every frame onto the exact palette.
        for (const ir::Frame* f : frames) {
          if (frames.size() == 1 && exact) {
            mapped.push_back(std::move(*exact));
            exact.reset();
          } else {
            QuantizeOptions none;
            none.dither = Dither::None;
            CVT_TRY_ASSIGN(Quantized qz, remap(*f, plan.palette, none));
            mapped.push_back(std::move(qz));
          }
        }
      }
    }
  }

  ChunkWriter out;
  out.signature_bytes();
  out.chunk("IHDR", header_chunk(width, height, plan, options.interlace && !animated));
  if (animated) {
    util::ByteWriter actl;
    actl.u32be(static_cast<std::uint32_t>(image.frames.size())).u32be(image.loop_count);
    out.chunk("acTL", actl.view());
  }
  write_metadata(out, image, options.keep_metadata);
  if (plan.color_type == 3) write_palette(out, plan);
  write_key(out, plan);

  const auto source_for = [&](std::size_t n) {
    Source src;
    src.width = width;
    src.height = height;
    src.stride_px = width;
    if (plan.color_type == 3) {
      src.indices = mapped[n].indices.data();
    } else if (plan.sixteen) {
      src.rgba16 = wide.data();
    } else {
      src.rgba8 = image.frames[n].rgba.data();
    }
    return src;
  };

  if (!animated) {
    CVT_TRY_ASSIGN(const Bytes z, compressed_data(plan, source_for(0), options.interlace, level, ctx));
    // Split very large data into 1 MiB chunks, as encoders customarily do.
    constexpr std::size_t piece = 1u << 20;
    for (std::size_t at = 0; at < z.size() || at == 0; at += piece) {
      out.chunk("IDAT", ByteView(z).subspan(at, std::min(piece, z.size() - at)));
      if (z.empty()) break;
    }
  } else {
    std::uint32_t sequence = 0;
    for (std::size_t n = 0; n < image.frames.size(); ++n) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(n) / image.frames.size(), "Encoding animation"));
      Rect rect{0, 0, width, height};
      if (n > 0) rect = changed_rect(image.frames[n - 1], image.frames[n]);
      util::ByteWriter fctl;
      const std::uint32_t delay = image.frames[n].delay_ms;
      fctl.u32be(sequence++).u32be(rect.w).u32be(rect.h).u32be(rect.x).u32be(rect.y);
      fctl.u16be(static_cast<std::uint16_t>(std::min<std::uint32_t>(delay, 65535))).u16be(1000);
      fctl.u8(0).u8(0);  // dispose none, blend source
      out.chunk("fcTL", fctl.view());
      Source src = source_for(n);
      src.x0 = rect.x;
      src.y0 = rect.y;
      src.width = rect.w;
      src.height = rect.h;
      CVT_TRY_ASSIGN(const Bytes z, compressed_data(plan, src, false, level, ctx));
      if (n == 0) {
        out.chunk("IDAT", z);
      } else {
        Bytes body;
        body.reserve(z.size() + 4);
        util::ByteWriter seq;
        seq.u32be(sequence++);
        append(body, seq.view());
        append(body, z);
        out.chunk("fdAT", body);
      }
    }
  }
  out.chunk("IEND", {});
  return out.take();
}

// --------------------------------------------------------------------------- engine glue

Result<ir::Image> read_png(ReadContext& ctx) { return decode(ctx.input, ctx); }

/// True when the caller asked for a lossy result (quality below 100 or a
/// non-"high" preset set explicitly) and did not insist on lossless.
int lossy_quality_from(const Options& values) {
  if (values.was_set(options::key_lossless) && options::resolved_lossless(values)) return 0;
  const bool quality_set = values.was_set(options::key_quality);
  const bool preset_set = values.was_set(options::key_preset);
  if (!quality_set && !(preset_set && options::resolved_preset(values) != options::Preset::High)) return 0;
  const int quality = options::resolved_quality(values);
  return quality >= 100 ? 0 : quality;
}

Result<Bytes> write_png(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  EncodeOptions options;
  options.compression_level = options::resolved_compression_level(ctx.options);
  options.lossy_quality = lossy_quality_from(ctx.options);
  const Dither dither = resolved_dither(ctx.options);
  options.dither = dither == Dither::None ? 0 : dither == Dither::Ordered ? 2 : 1;
  const std::string depth = ctx.options.get_enum("bit_depth", "auto", {"auto", "8", "16"});
  options.bit_depth = depth == "8" ? 8 : depth == "16" ? 16 : 0;
  options.interlace = ctx.options.get_bool("interlace", false);
  options.keep_metadata = prepared.keep_metadata();
  return encode(*prepared, options, ctx);
}

std::vector<OptionSpec> png_write_options() {
  std::vector<OptionSpec> specs = lossless_writer_options();
  specs.insert(specs.begin(), options::quality());
  specs.push_back(options::lossless());
  specs.push_back(dither_option());
  specs.push_back(OptionSpec::choice("bit_depth", "Bit depth", "auto", {"auto", "8", "16"},
                                     "Bits per sample: auto keeps 16 bits for 16-bit sources and reduces "
                                     "losslessly where possible.")
                      .advanced());
  specs.push_back(OptionSpec::boolean("interlace", "Interlace (Adam7)", false,
                                      "Progressive display while downloading; makes the file slightly larger.")
                      .advanced());
  // Document how quality works for this lossless format.
  specs.front().description =
      "PNG is lossless by default. Setting a quality below 100 (or the balanced/small preset) reduces the picture "
      "to a palette of at most 256 colours when that keeps the quality, which makes files much smaller.";
  return specs;
}

int sniff_apng(std::span<const std::byte> data, std::string_view /*filename*/) {
  if (data.size() < 8 || as_chars(data.first(8)) != signature) return 0;
  util::ByteReader reader(data.subspan(8));
  for (int i = 0; i < 64 && reader.remaining() >= 12; ++i) {
    const std::uint32_t length = reader.u32be();
    const std::string_view type = reader.chars(4);
    if (type == "acTL") return 97;
    if (type == "IDAT" || !reader.skip(length) || !reader.skip(4)) return 0;
  }
  return 0;
}

// --------------------------------------------------------------------------- "png" video codec

class PngVideoDecoder final : public codec::VideoDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    timescale_ = params.timescale == 0 ? 1000 : params.timescale;
    return {};
  }
  Result<void> send_packet(const codec::Packet& packet) override {
    Context ctx;
    CVT_TRY_ASSIGN(ir::Image image, decode(packet.bytes, ctx));
    codec::VideoFrame frame;
    frame.width = image.width();
    frame.height = image.height();
    frame.format = codec::PixelFormat::RGBA8;
    frame.planes.resize(1);
    frame.planes[0].stride = static_cast<std::size_t>(frame.width) * 4;
    frame.planes[0].data = std::move(image.frames[0].rgba);
    frame.pts_ms = packet.pts * 1000 / static_cast<std::int64_t>(timescale_);
    frame.duration_ms = packet.duration * 1000 / static_cast<std::int64_t>(timescale_);
    pending_ = std::move(frame);
    return {};
  }
  Result<std::optional<codec::VideoFrame>> receive_frame() override {
    std::optional<codec::VideoFrame> out = std::move(pending_);
    pending_.reset();
    return out;
  }
  Result<void> flush() override { return {}; }

 private:
  std::uint32_t timescale_ = 1000;
  std::optional<codec::VideoFrame> pending_;
};

class PngVideoEncoder final : public codec::VideoEncoder {
 public:
  Result<void> configure(const codec::VideoEncodeParams& params) override {
    if (params.width == 0 || params.height == 0) return fail(Status::InvalidArgument, "PNG video needs a frame size");
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 90000;
    level_ = params.preset == codec::EncodePreset::Fast ? 3 : params.preset == codec::EncodePreset::Small ? 9 : 6;
    return {};
  }
  Result<void> send_frame(const codec::VideoFrame& frame) override {
    CVT_TRY_ASSIGN(const codec::VideoFrame rgba, codec::to_rgba(frame));
    ir::Image image;
    ir::Frame f;
    f.width = rgba.width;
    f.height = rgba.height;
    f.rgba.resize(static_cast<std::size_t>(rgba.width) * rgba.height * 4);
    for (std::uint32_t y = 0; y < rgba.height; ++y) {
      std::memcpy(f.rgba.data() + static_cast<std::size_t>(y) * rgba.width * 4,
                  rgba.planes[0].data.data() + y * rgba.planes[0].stride, static_cast<std::size_t>(rgba.width) * 4);
    }
    image.frames.push_back(std::move(f));
    EncodeOptions options;
    options.compression_level = level_;
    Context ctx;
    CVT_TRY_ASSIGN(Bytes bytes, encode(image, options, ctx));
    codec::Packet packet;
    packet.bytes = std::move(bytes);
    packet.pts = frame.pts_ms * static_cast<std::int64_t>(params_.timescale) / 1000;
    packet.dts = packet.pts;
    packet.duration = frame.duration_ms * static_cast<std::int64_t>(params_.timescale) / 1000;
    packet.keyframe = true;
    pending_.push_back(std::move(packet));
    return {};
  }
  Result<std::optional<codec::Packet>> receive_packet() override {
    if (pending_.empty()) return std::optional<codec::Packet>{};
    codec::Packet packet = std::move(pending_.front());
    pending_.erase(pending_.begin());
    return std::optional<codec::Packet>(std::move(packet));
  }
  Bytes extradata() const override { return {}; }
  Result<void> flush() override { return {}; }

 private:
  codec::VideoEncodeParams params_;
  int level_ = 6;
  std::vector<codec::Packet> pending_;
};

}  // namespace

}  // namespace cvt::image::png

namespace cvt::image {

void register_png(Registry& registry) {
  FormatInfo info;
  info.id = "png";
  info.name = "PNG image";
  info.description = "Lossless compressed raster image with transparency.";
  info.category = Category::Image;
  info.extensions = {"png"};
  info.mime = "image/png";
  info.popularity = 95;
  info.signatures = {Signature::at(0, "89 'PNG' 0D 0A 1A 0A")};
  registry.add_format(std::move(info));

  FormatInfo apng;
  apng.id = "apng";
  apng.name = "Animated PNG";
  apng.description = "PNG with animation frames (APNG).";
  apng.category = Category::Image;
  apng.extensions = {"apng"};
  apng.mime = "image/apng";
  apng.popularity = 30;
  apng.sniff = png::sniff_apng;
  registry.add_format(std::move(apng));

  for (const char* id : {"png", "apng"}) {
    ImageCodecSpec spec;
    spec.format_id = id;
    spec.read = reader(png::read_png);
    spec.write = writer(png::write_png);
    spec.write_options = png::png_write_options();
    add_image_codec(registry, std::move(spec));
  }

  registry.media().register_video_decoder(codec::id::png, [] { return std::make_unique<png::PngVideoDecoder>(); });
  registry.media().register_video_encoder(codec::id::png, [] { return std::make_unique<png::PngVideoEncoder>(); });
}

}  // namespace cvt::image
