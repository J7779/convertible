// Convertible core - codec_legacy family: simple RGB and palette codecs.
//
//   qtrle     QuickTime Animation ('rle '): depths 1/2/4/8 (palette or grey),
//             16 (RGB555), 24 (RGB), 32 (ARGB); decoder and encoder (24/32 bit)
//   msrle     Microsoft RLE (BI_RLE8, BI_RLE4), bottom-up, palette
//   msvideo1  Microsoft Video 1 / CRAM, 8-bit (palette) and 16-bit (RGB555)
//   rpza      QuickTime Video "road pizza" (RGB555, 4x4 blocks)
//   smc       QuickTime Graphics (8-bit palette, 4x4 blocks with colour caches)
//   cinepak   Cinepak (cvid), colour and grey codebooks, intra/inter strips
//
// Container conventions (see codec_notes.hpp): `CodecParams::bits_per_sample`
// carries the coded depth (BITMAPINFOHEADER biBitCount, QuickTime sample
// description depth: 33/34/36/40 are the grey variants of 1/2/4/8); a palette
// travels as extradata of N x (B, G, R, 0) entries, the AVI RGBQUAD layout
// (MOV colour tables are converted to it). All decoders output RGBA8.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "families/codec_legacy/codecs.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/dct.hpp"
#include "util/bytes.hpp"

namespace cvt::legacy {

namespace {

using Rgba = std::array<std::uint8_t, 4>;

[[nodiscard]] Rgba from555(std::uint32_t v) {
  const auto c = [](std::uint32_t x) { return static_cast<std::uint8_t>((x << 3) | (x >> 2)); };
  return {c((v >> 10) & 31), c((v >> 5) & 31), c(v & 31), 255};
}

/// Palette from extradata (B, G, R, x quads), or a grey ramp.
[[nodiscard]] std::array<Rgba, 256> read_palette(const Bytes& extradata, int depth) {
  std::array<Rgba, 256> palette{};
  const int colors = depth >= 1 && depth <= 8 ? 1 << depth : 256;
  for (int i = 0; i < 256; ++i) {
    const auto g = static_cast<std::uint8_t>(colors > 1 ? 255 - std::min(255, i * 255 / (colors - 1)) : 255);
    palette[static_cast<std::size_t>(i)] = {g, g, g, 255};
  }
  const auto data = as_u8(ByteView(extradata));
  for (std::size_t i = 0; i < 256 && i * 4 + 3 < data.size(); ++i) {
    palette[i] = {data[i * 4 + 2], data[i * 4 + 1], data[i * 4], 255};
  }
  return palette;
}

/// An RGBA8 canvas kept between frames (delta codecs).
struct Canvas {
  VideoFrame frame;
  void ensure(std::uint32_t w, std::uint32_t h) {
    if (frame.width != w || frame.height != h || frame.planes.empty()) {
      frame = VideoFrame::allocate(PixelFormat::RGBA8, w, h);
      for (std::size_t i = 3; i < frame.planes[0].data.size(); i += 4) frame.planes[0].data[i] = 255;
    }
  }
  [[nodiscard]] std::uint8_t* px(std::uint32_t x, std::uint32_t y) {
    return frame.planes[0].data.data() + static_cast<std::size_t>(y) * frame.planes[0].stride + static_cast<std::size_t>(x) * 4;
  }
  void set(std::uint32_t x, std::uint32_t y, const Rgba& c) {
    if (x < frame.width && y < frame.height) ::cvt::util::copy_bytes(px(x, y), c.data(), 4);
  }
};

class CanvasDecoder : public SimpleDecoder {
 protected:
  Result<void> on_configure() override {
    if (params_.width == 0 || params_.height == 0) {
      return fail(Status::Decode, "this codec needs the picture size from the container");
    }
    canvas_.ensure(params_.width, params_.height);
    return {};
  }
  Canvas canvas_;
};

// ---- QuickTime Animation ---------------------------------------------------------------

class QtrleDecoder final : public CanvasDecoder {
 protected:
  Result<void> on_configure() override {
    CVT_TRY(CanvasDecoder::on_configure());
    depth_ = params_.bits_per_sample == 0 ? 24 : params_.bits_per_sample;
    const int bits = depth_ > 32 ? depth_ - 32 : depth_;
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && depth_ != 16 && depth_ != 24 && depth_ != 32) {
      return fail(Status::Decode, "unsupported QuickTime Animation depth {}", depth_);
    }
    palette_ = read_palette(params_.extradata, depth_ > 32 ? depth_ - 32 : depth_);
    if (depth_ > 32 && params_.extradata.empty()) palette_ = read_palette({}, depth_ - 32);
    if (depth_ == 1 && params_.extradata.empty()) {
      palette_[0] = {255, 255, 255, 255};
      palette_[1] = {0, 0, 0, 255};
    }
    return {};
  }

  Result<VideoFrame> decode(const Packet& packet) override {
    util::ByteReader r(packet.bytes);
    if (packet.bytes.size() < 8) return canvas_.frame;  // no change
    r.skip(4);  // chunk size
    const std::uint32_t header = r.u16be();
    std::uint32_t start = 0;
    std::uint32_t lines = params_.height;
    if ((header & 0x0008) != 0) {
      start = r.u16be();
      r.skip(2);
      lines = r.u16be();
      r.skip(2);
    }
    if (!r.ok() || start + lines > params_.height) return fail(Status::Decode, "damaged QuickTime Animation header");
    const int bits = depth_ > 32 ? depth_ - 32 : depth_;
    if (bits == 1) {
      CVT_TRY(decode_1bpp(r, start, lines));
      return canvas_.frame;
    }
    // Pixels per coding unit and bytes per unit.
    int unit_pixels = 1;
    int unit_bytes = depth_ / 8;
    if (bits <= 8 && depth_ != 16 && depth_ != 24 && depth_ != 32) {
      unit_bytes = 4;
      unit_pixels = 32 / bits;
    }
    unit_bytes = std::clamp(unit_bytes, 1, 4);  // 1..4 bytes per unit for every valid depth; keeps the copies below in bounds
    const std::int64_t width = params_.width;
    for (std::uint32_t line = start; line < start + lines; ++line) {
      std::int64_t x = (static_cast<std::int64_t>(r.u8()) - 1) * unit_pixels;
      while (true) {
        if (!r.ok()) return fail(Status::Decode, "truncated QuickTime Animation frame");
        const auto code = static_cast<std::int8_t>(r.u8());
        if (code == -1) break;
        if (code == 0) {
          x += (static_cast<std::int64_t>(r.u8()) - 1) * unit_pixels;
          continue;
        }
        const int count = code > 0 ? code : -code;
        std::array<std::uint8_t, 4> repeated{};
        if (code < 0) {
          for (int b = 0; b < unit_bytes; ++b) repeated[static_cast<std::size_t>(b)] = r.u8();
        }
        for (int k = 0; k < count; ++k) {
          std::array<std::uint8_t, 4> unit = repeated;
          if (code > 0) {
            for (int b = 0; b < unit_bytes; ++b) unit[static_cast<std::size_t>(b)] = r.u8();
          }
          if (x < 0 || x + unit_pixels > width + unit_pixels) return fail(Status::Decode, "QuickTime Animation run outside the picture");
          put_unit(unit, static_cast<std::uint32_t>(std::max<std::int64_t>(0, x)), line, bits);
          x += unit_pixels;
        }
      }
    }
    return canvas_.frame;
  }

 private:
  void put_unit(const std::array<std::uint8_t, 4>& u, std::uint32_t x, std::uint32_t y, int bits) {
    switch (depth_) {
      case 16: canvas_.set(x, y, from555(static_cast<std::uint32_t>(u[0]) << 8 | u[1])); return;
      case 24: canvas_.set(x, y, {u[0], u[1], u[2], 255}); return;
      case 32: canvas_.set(x, y, {u[1], u[2], u[3], u[0]}); return;
      default: break;
    }
    // Palette indices, most significant bits first.
    const int per_byte = 8 / bits;
    for (int b = 0; b < 4; ++b) {
      for (int k = 0; k < per_byte; ++k) {
        const int shift = 8 - bits * (k + 1);
        const int index = (u[static_cast<std::size_t>(b)] >> shift) & ((1 << bits) - 1);
        canvas_.set(x + static_cast<std::uint32_t>(b * per_byte + k), y, palette_[static_cast<std::size_t>(index)]);
      }
    }
  }

  Result<void> decode_1bpp(util::ByteReader& r, std::uint32_t start, std::uint32_t lines) {
    std::uint32_t y = start;
    std::int64_t x = 0;
    std::uint32_t remaining = lines;
    while (remaining > 0) {
      const std::uint8_t skip = r.u8();
      const auto code = static_cast<std::int8_t>(r.u8());
      if (!r.ok()) return fail(Status::Decode, "truncated QuickTime Animation frame");
      if (code == 0) break;
      if ((skip & 0x80) != 0) {
        --remaining;
        ++y;
        x = 16 * (skip & 0x7F);
      } else {
        x += 16 * skip;
      }
      const int count = code > 0 ? code * 2 : -code;
      std::uint8_t pair[2] = {0, 0};
      if (code < 0) {
        pair[0] = r.u8();
        pair[1] = r.u8();
      }
      for (int k = 0; k < count; ++k) {
        const int bytes = code < 0 ? 2 : 1;
        for (int b = 0; b < bytes; ++b) {
          const std::uint8_t v = code < 0 ? pair[b] : r.u8();
          for (int bit = 0; bit < 8; ++bit) {
            if (y < params_.height + 1 && y > 0) canvas_.set(static_cast<std::uint32_t>(x), y - 1, palette_[static_cast<std::size_t>((v >> (7 - bit)) & 1)]);
            ++x;
          }
        }
        if (!r.ok()) return fail(Status::Decode, "truncated QuickTime Animation frame");
      }
    }
    return {};
  }

  int depth_ = 24;
  std::array<Rgba, 256> palette_{};
};

/// QuickTime Animation encoder: 32-bit ARGB when the configured pixel format
/// is RGBA8 (alpha is kept), 24-bit RGB otherwise; the muxer writes the same
/// depth into the sample description.
/// Key frames code every line; delta frames only the changed lines, with
/// skip codes over unchanged pixels. Lossless.
class QtrleEncoder final : public SimpleEncoder {
 protected:
  Result<void> on_configure() override {
    if (params_.width > 65535 || params_.height > 65535) return fail(Status::InvalidArgument, "picture too large");
    keyint_ = params_.keyframe_interval > 0 ? params_.keyframe_interval : 60;
    depth_ = params_.pixel_format == PixelFormat::RGBA8 ? 32 : 24;
    return {};
  }

  Result<Bytes> encode(const VideoFrame& frame, bool& keyframe) override {
    CVT_TRY_ASSIGN(const VideoFrame rgba, to_format(frame, PixelFormat::RGBA8, {}));
    keyframe = frame_index_ % keyint_ == 0 || previous_.empty();
    const std::uint32_t w = params_.width;
    const std::uint32_t h = params_.height;
    const auto row = [&](const std::vector<std::uint8_t>& image, std::uint32_t y) { return image.data() + static_cast<std::size_t>(y) * w * 4; };
    std::vector<std::uint8_t> current(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
      ::cvt::util::copy_bytes(current.data() + static_cast<std::size_t>(y) * w * 4, rgba.planes[0].data.data() + y * rgba.planes[0].stride, static_cast<std::size_t>(w) * 4);
    }
    std::uint32_t first = 0;
    std::uint32_t last = h;  // exclusive
    if (!keyframe) {
      while (first < h && ::cvt::util::compare_bytes(row(current, first), row(previous_, first), static_cast<std::size_t>(w) * 4) == 0) ++first;
      while (last > first && ::cvt::util::compare_bytes(row(current, last - 1), row(previous_, last - 1), static_cast<std::size_t>(w) * 4) == 0) --last;
    }
    util::ByteWriter out;
    if (first == last) {
      out.u32be(4);  // no change
      previous_ = std::move(current);
      return out.take();
    }
    out.u32be(0);  // size, patched below
    out.u16be(0x0008);
    out.u16be(static_cast<std::uint16_t>(first));
    out.u16be(0);
    out.u16be(static_cast<std::uint16_t>(last - first));
    out.u16be(0);
    const int bpp = depth_ / 8;
    const auto pixel = [&](const std::uint8_t* p, util::ByteWriter& o) {
      if (bpp == 4) o.u8(p[3]);
      o.u8(p[0]).u8(p[1]).u8(p[2]);
    };
    for (std::uint32_t y = first; y < last; ++y) {
      const std::uint8_t* cur = row(current, y);
      const std::uint8_t* old = keyframe ? nullptr : row(previous_, y);
      std::uint32_t x = 0;
      // Leading skip.
      std::uint32_t skip = 0;
      if (old != nullptr) {
        while (x + skip < w && ::cvt::util::compare_bytes(cur + (x + skip) * 4, old + (x + skip) * 4, 4) == 0) ++skip;
      }
      std::uint32_t pending_skip = skip;
      x += skip;
      out.u8(static_cast<std::uint8_t>(std::min<std::uint32_t>(pending_skip, 254) + 1));
      pending_skip -= std::min<std::uint32_t>(pending_skip, 254);
      while (pending_skip > 0) {
        const std::uint32_t s = std::min<std::uint32_t>(pending_skip, 254);
        out.u8(0).u8(static_cast<std::uint8_t>(s + 1));
        pending_skip -= s;
      }
      while (x < w) {
        // Unchanged pixels: skip code.
        if (old != nullptr) {
          std::uint32_t same = 0;
          while (x + same < w && ::cvt::util::compare_bytes(cur + (x + same) * 4, old + (x + same) * 4, 4) == 0) ++same;
          if (same >= 2 || (same > 0 && x + same == w)) {
            if (x + same == w) break;  // the rest of the line is unchanged
            std::uint32_t s = same;
            while (s > 0) {
              const std::uint32_t step = std::min<std::uint32_t>(s, 254);
              out.u8(0).u8(static_cast<std::uint8_t>(step + 1));
              s -= step;
            }
            x += same;
            continue;
          }
        }
        // Run of identical pixels.
        std::uint32_t run = 1;
        while (x + run < w && run < 128 && ::cvt::util::compare_bytes(cur + x * 4, cur + (x + run) * 4, 4) == 0) ++run;
        if (run >= 3) {
          out.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(-static_cast<int>(run))));
          pixel(cur + x * 4, out);
          x += run;
          continue;
        }
        // Literal pixels up to the next run or unchanged stretch.
        std::uint32_t count = 0;
        while (x + count < w && count < 127) {
          const std::uint8_t* p = cur + (x + count) * 4;
          if (x + count + 2 < w && ::cvt::util::compare_bytes(p, p + 4, 4) == 0 && ::cvt::util::compare_bytes(p, p + 8, 4) == 0) break;
          if (old != nullptr && x + count + 1 < w && ::cvt::util::compare_bytes(p, old + (x + count) * 4, 4) == 0 &&
              ::cvt::util::compare_bytes(p + 4, old + (x + count + 1) * 4, 4) == 0) {
            break;
          }
          ++count;
        }
        if (count == 0) count = 1;
        out.u8(static_cast<std::uint8_t>(count));
        for (std::uint32_t k = 0; k < count; ++k) pixel(cur + (x + k) * 4, out);
        x += count;
      }
      out.u8(0xFF);  // end of line
    }
    out.u8(0);  // terminating skip code
    Bytes bytes = out.take();
    const std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
    for (int i = 0; i < 4; ++i) bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>(size >> (24 - 8 * i));
    previous_ = std::move(current);
    return bytes;
  }

 private:
  int depth_ = 24;
  std::uint64_t keyint_ = 60;
  std::vector<std::uint8_t> previous_;
};

// ---- Microsoft RLE ------------------------------------------------------------------------

class MsrleDecoder final : public CanvasDecoder {
 protected:
  Result<void> on_configure() override {
    CVT_TRY(CanvasDecoder::on_configure());
    depth_ = params_.bits_per_sample == 0 ? 8 : params_.bits_per_sample;
    if (depth_ != 4 && depth_ != 8) return fail(Status::Decode, "unsupported Microsoft RLE depth {}", depth_);
    palette_ = read_palette(params_.extradata, depth_);
    indices_.assign(static_cast<std::size_t>(params_.width) * params_.height, 0);
    return {};
  }

  Result<VideoFrame> decode(const Packet& packet) override {
    util::ByteReader r(packet.bytes);
    const std::int64_t w = params_.width;
    const std::int64_t h = params_.height;
    std::int64_t x = 0;
    std::int64_t line = 0;  // counted from the bottom
    const auto put = [&](int index) {
      if (x >= 0 && x < w && line < h) indices_[static_cast<std::size_t>((h - 1 - line) * w + x)] = static_cast<std::uint8_t>(index);
      ++x;
    };
    bool done = false;
    while (!done && line < h) {
      const std::uint8_t count = r.u8();
      const std::uint8_t value = r.u8();
      if (!r.ok()) break;  // a missing end-of-bitmap marker is tolerated
      if (count > 0) {
        for (int k = 0; k < count && x < w; ++k) {
          if (depth_ == 8) put(value);
          else put((k & 1) == 0 ? value >> 4 : value & 15);
        }
        continue;
      }
      switch (value) {
        case 0: x = 0; ++line; break;
        case 1: done = true; break;
        case 2: x += r.u8(); line += r.u8(); break;
        default: {
          const int n = value;
          const int bytes = depth_ == 8 ? n : (n + 1) / 2;
          for (int k = 0; k < n; ++k) {
            if (depth_ == 8) {
              put(r.u8());
            } else {
              const std::uint8_t b = r.u8();
              put(b >> 4);
              if (++k < n) put(b & 15);
            }
          }
          if ((bytes & 1) != 0) r.skip(1);  // word alignment
          if (!r.ok()) return fail(Status::Decode, "truncated Microsoft RLE frame");
          break;
        }
      }
    }
    VideoFrame& f = canvas_.frame;
    for (std::int64_t y = 0; y < h; ++y) {
      for (std::int64_t xx = 0; xx < w; ++xx) {
        const Rgba& c = palette_[indices_[static_cast<std::size_t>(y * w + xx)]];
        ::cvt::util::copy_bytes(f.planes[0].data.data() + static_cast<std::size_t>(y) * f.planes[0].stride + static_cast<std::size_t>(xx) * 4, c.data(), 4);
      }
    }
    return f;
  }

 private:
  int depth_ = 8;
  std::array<Rgba, 256> palette_{};
  std::vector<std::uint8_t> indices_;
};

// ---- Microsoft Video 1 --------------------------------------------------------------------------

class MsVideo1Decoder final : public CanvasDecoder {
 protected:
  Result<void> on_configure() override {
    CVT_TRY(CanvasDecoder::on_configure());
    depth_ = params_.bits_per_sample == 0 ? (params_.extradata.size() >= 4 ? 8 : 16) : params_.bits_per_sample;
    if (depth_ != 8 && depth_ != 16) return fail(Status::Decode, "unsupported Microsoft Video 1 depth {}", depth_);
    palette_ = read_palette(params_.extradata, 8);
    return {};
  }

  Result<VideoFrame> decode(const Packet& packet) override {
    util::ByteReader r(packet.bytes);
    const std::uint32_t blocks_wide = params_.width / 4;
    const std::uint32_t blocks_high = params_.height / 4;
    std::uint64_t total = std::uint64_t{blocks_wide} * blocks_high;
    std::uint64_t skip = 0;
    const bool wide = depth_ == 16;
    const auto color = [&](std::uint32_t v) { return wide ? from555(v & 0x7FFF) : palette_[v & 0xFF]; };
    for (std::uint32_t by = blocks_high; by > 0; --by) {
      for (std::uint32_t bx = 0; bx < blocks_wide; ++bx) {
        if (skip > 0) {
          --skip;
          --total;
          continue;
        }
        const std::uint8_t a = r.u8();
        const std::uint8_t b = r.u8();
        if (!r.ok()) return canvas_.frame;  // short frames leave the rest unchanged
        const std::uint32_t top = (by - 1) * 4;
        // Pixel (px, py) of the block, py counted from the bottom row.
        const auto set = [&](int px, int py, const Rgba& c) { canvas_.set(bx * 4 + static_cast<std::uint32_t>(px), top + 3 - static_cast<std::uint32_t>(py), c); };
        if (a == 0 && b == 0 && total == 0) return canvas_.frame;
        if ((b & 0xFC) == 0x84) {
          skip = ((static_cast<std::uint64_t>(b) - 0x84) << 8) + a - 1;
        } else if (b < 0x80 || (!wide && b >= 0x90)) {
          std::uint32_t flags = static_cast<std::uint32_t>(b) << 8 | a;
          std::array<std::uint32_t, 8> colors{};
          const auto read_color = [&]() -> std::uint32_t { return wide ? r.u16le() : r.u8(); };
          colors[0] = read_color();
          colors[1] = read_color();
          const bool eight = wide ? (colors[0] & 0x8000) != 0 : false;
          if (wide && eight) {
            for (int k = 2; k < 8; ++k) colors[static_cast<std::size_t>(k)] = read_color();
          }
          if (!wide && b >= 0x90) {
            // 8-bit 8-colour blocks are signalled by the flag range 0x9000..0xFFFF in 8-bit mode.
            for (int k = 2; k < 8; ++k) colors[static_cast<std::size_t>(k)] = read_color();
          }
          const bool use8 = wide ? eight : (b >= 0x90);
          for (int py = 0; py < 4; ++py) {
            for (int px = 0; px < 4; ++px, flags >>= 1) {
              const int pick = (flags & 1) ^ 1;
              const std::size_t index = use8 ? static_cast<std::size_t>(((py & 2) << 1) + (px & 2) + pick) : static_cast<std::size_t>(pick);
              set(px, py, color(colors[index]));
            }
          }
        } else {
          const Rgba c = color(wide ? (static_cast<std::uint32_t>(b) << 8 | a) : a);
          for (int py = 0; py < 4; ++py) {
            for (int px = 0; px < 4; ++px) set(px, py, c);
          }
        }
        --total;
      }
    }
    return canvas_.frame;
  }

 private:
  int depth_ = 16;
  std::array<Rgba, 256> palette_{};
};

// ---- QuickTime Video (RPZA) ------------------------------------------------------------------------

class RpzaDecoder final : public CanvasDecoder {
 protected:
  Result<VideoFrame> decode(const Packet& packet) override {
    util::ByteReader r(packet.bytes);
    if (r.u8() != 0xE1) return fail(Status::Decode, "not a QuickTime Video (rpza) frame");
    r.skip(3);  // chunk size
    const std::uint32_t blocks_wide = (params_.width + 3) / 4;
    const std::uint64_t total = std::uint64_t{blocks_wide} * ((params_.height + 3) / 4);
    std::uint64_t block = 0;
    const auto paint = [&](std::uint64_t index, const auto& pixel) {
      const std::uint32_t bx = static_cast<std::uint32_t>(index % blocks_wide) * 4;
      const std::uint32_t by = static_cast<std::uint32_t>(index / blocks_wide) * 4;
      for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) canvas_.set(bx + x, by + y, pixel(x, y));
      }
    };
    while (block < total && !r.at_end()) {
      std::uint32_t opcode = r.u8();
      std::uint64_t n = (opcode & 0x1F) + 1;
      std::uint32_t color_a = 0;
      if ((opcode & 0x80) == 0) {
        color_a = opcode << 8 | r.u8();
        opcode = 0;
        if (r.remaining() > 0 && (as_u8(packet.bytes)[r.position()] & 0x80) != 0) {
          opcode = 0x20;
          n = 1;
        }
      }
      n = std::min(n, total - block);
      switch (opcode & 0xE0) {
        case 0x80: block += n; break;
        case 0xA0: {
          const Rgba c = from555(r.u16be());
          for (std::uint64_t k = 0; k < n; ++k) paint(block++, [&](std::uint32_t, std::uint32_t) { return c; });
          break;
        }
        case 0xC0:
          color_a = r.u16be();
          [[fallthrough]];
        case 0x20: {
          const std::uint32_t color_b = r.u16be();
          std::array<Rgba, 4> four{};
          four[0] = from555(color_b);
          four[3] = from555(color_a);
          for (int mix = 1; mix <= 2; ++mix) {
            const int wa = mix == 1 ? 11 : 21;
            const int wb = 32 - wa;
            std::uint32_t c = 0;
            for (int shift = 0; shift <= 10; shift += 5) {
              const std::uint32_t ca = (color_a >> shift) & 31;
              const std::uint32_t cb = (color_b >> shift) & 31;
              c |= ((static_cast<std::uint32_t>(wa) * ca + static_cast<std::uint32_t>(wb) * cb) >> 5) << shift;
            }
            four[static_cast<std::size_t>(mix)] = from555(c);
          }
          for (std::uint64_t k = 0; k < n; ++k) {
            std::array<std::uint8_t, 4> rows{};
            for (auto& row : rows) row = r.u8();
            paint(block++, [&](std::uint32_t x, std::uint32_t y) { return four[(rows[y] >> (6 - 2 * x)) & 3]; });
          }
          break;
        }
        default: {  // 0x00: 16 colours, the first already read
          std::array<Rgba, 16> c{};
          c[0] = from555(color_a);
          for (std::size_t k = 1; k < 16; ++k) c[k] = from555(r.u16be());
          paint(block++, [&](std::uint32_t x, std::uint32_t y) { return c[y * 4 + x]; });
          break;
        }
      }
      if (!r.ok()) return fail(Status::Decode, "truncated QuickTime Video (rpza) frame");
    }
    return canvas_.frame;
  }
};

// ---- QuickTime Graphics (SMC) ---------------------------------------------------------------------------

class SmcDecoder final : public CanvasDecoder {
 protected:
  Result<void> on_configure() override {
    CVT_TRY(CanvasDecoder::on_configure());
    palette_ = read_palette(params_.extradata, 8);
    indices_.assign(static_cast<std::size_t>((params_.width + 3) / 4 * 4) * ((params_.height + 3) / 4 * 4), 0);
    return {};
  }

  Result<VideoFrame> decode(const Packet& packet) override {
    util::ByteReader r(packet.bytes);
    r.skip(4);  // flags + chunk size
    const std::uint32_t bw = (params_.width + 3) / 4;
    const std::uint64_t total = std::uint64_t{bw} * ((params_.height + 3) / 4);
    const std::uint32_t stride = bw * 4;
    // The colour caches persist across frames; only the insertion points restart.
    auto& cache2 = cache2_;
    auto& cache4 = cache4_;
    auto& cache8 = cache8_;
    std::size_t next2 = 0;
    std::size_t next4 = 0;
    std::size_t next8 = 0;
    std::uint64_t block = 0;
    const auto origin = [&](std::uint64_t index) -> std::size_t {
      return static_cast<std::size_t>((index / bw) * 4 * stride + (index % bw) * 4);
    };
    const auto copy_block = [&](std::uint64_t from, std::uint64_t to) {
      for (std::uint32_t y = 0; y < 4; ++y) {
        ::cvt::util::copy_bytes(indices_.data() + origin(to) + y * stride, indices_.data() + origin(from) + y * stride, 4);
      }
    };
    const auto paint = [&](std::uint64_t index, const auto& pixel) {
      for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) indices_[origin(index) + y * stride + x] = pixel(x, y);
      }
    };
    while (block < total && !r.at_end()) {
      const std::uint32_t opcode = r.u8();
      std::uint64_t n = (opcode & 0x0F) + 1;
      if ((opcode & 0x10) != 0 && (opcode & 0xF0) <= 0x70) n = static_cast<std::uint64_t>(r.u8()) + 1;
      if (!r.ok()) break;
      switch (opcode & 0xF0) {
        case 0x00:
        case 0x10: block += n; break;
        case 0x20:
        case 0x30:
          if (block == 0) return fail(Status::Decode, "QuickTime Graphics repeats a block before the first");
          for (std::uint64_t k = 0; k < n && block < total; ++k, ++block) copy_block(block - 1, block);
          break;
        case 0x40:
        case 0x50: {
          if (block < 2) return fail(Status::Decode, "QuickTime Graphics repeats blocks before the first");
          const std::uint64_t a = block - 2;
          const std::uint64_t b = block - 1;
          for (std::uint64_t k = 0; k < n && block < total; ++k) {
            copy_block(a, block++);
            if (block < total) copy_block(b, block++);
          }
          break;
        }
        case 0x60:
        case 0x70: {
          const std::uint8_t c = r.u8();
          for (std::uint64_t k = 0; k < n && block < total; ++k) paint(block++, [&](std::uint32_t, std::uint32_t) { return c; });
          break;
        }
        case 0x80:
        case 0x90: {
          std::size_t entry;
          if ((opcode & 0xF0) == 0x80) {
            entry = next2;
            cache2[entry] = {r.u8(), r.u8()};
            next2 = (next2 + 1) & 255;
          } else {
            entry = r.u8();
          }
          for (std::uint64_t k = 0; k < n && block < total; ++k) {
            const std::uint32_t flags = r.u16be();
            paint(block++, [&](std::uint32_t x, std::uint32_t y) { return cache2[entry][(flags >> (15 - (y * 4 + x))) & 1]; });
          }
          break;
        }
        case 0xA0:
        case 0xB0: {
          std::size_t entry;
          if ((opcode & 0xF0) == 0xA0) {
            entry = next4;
            for (auto& c : cache4[entry]) c = r.u8();
            next4 = (next4 + 1) & 255;
          } else {
            entry = r.u8();
          }
          for (std::uint64_t k = 0; k < n && block < total; ++k) {
            const std::uint32_t flags = r.u32be();
            paint(block++, [&](std::uint32_t x, std::uint32_t y) { return cache4[entry][(flags >> (30 - 2 * (y * 4 + x))) & 3]; });
          }
          break;
        }
        case 0xC0:
        case 0xD0: {
          std::size_t entry;
          if ((opcode & 0xF0) == 0xC0) {
            entry = next8;
            for (auto& c : cache8[entry]) c = r.u8();
            next8 = (next8 + 1) & 255;
          } else {
            entry = r.u8();
          }
          for (std::uint64_t k = 0; k < n && block < total; ++k) {
            std::array<std::uint8_t, 6> b{};
            for (auto& v : b) v = r.u8();
            // 48 bits = 16 x 3-bit indices in four 12-bit rows: row 0 = b0 and the
            // high nibble of b1, row 1 = b2 and b3's high nibble, row 2 = b4 and
            // b5's high nibble, row 3 = the low nibbles of b1, b3 and b5.
            const std::uint32_t rows[4] = {static_cast<std::uint32_t>(b[0]) << 4 | b[1] >> 4,
                                           static_cast<std::uint32_t>(b[2]) << 4 | b[3] >> 4,
                                           static_cast<std::uint32_t>(b[4]) << 4 | b[5] >> 4,
                                           static_cast<std::uint32_t>(b[1] & 15) << 8 | (b[3] & 15u) << 4 | (b[5] & 15u)};
            paint(block++, [&](std::uint32_t x, std::uint32_t y) { return cache8[entry][(rows[y] >> (9 - 3 * x)) & 7]; });
          }
          break;
        }
        case 0xE0: {
          for (std::uint64_t k = 0; k < n && block < total; ++k) {
            std::array<std::uint8_t, 16> c{};
            for (auto& v : c) v = r.u8();
            paint(block++, [&](std::uint32_t x, std::uint32_t y) { return c[y * 4 + x]; });
          }
          break;
        }
        default: return fail(Status::Decode, "invalid QuickTime Graphics opcode {:#x}", opcode);
      }
      if (!r.ok()) return fail(Status::Decode, "truncated QuickTime Graphics frame");
    }
    VideoFrame& f = canvas_.frame;
    for (std::uint32_t y = 0; y < params_.height; ++y) {
      for (std::uint32_t x = 0; x < params_.width; ++x) {
        ::cvt::util::copy_bytes(f.planes[0].data.data() + y * f.planes[0].stride + x * 4, palette_[indices_[y * stride + x]].data(), 4);
      }
    }
    return f;
  }

 private:
  std::array<Rgba, 256> palette_{};
  std::vector<std::uint8_t> indices_;
  std::array<std::array<std::uint8_t, 2>, 256> cache2_{};
  std::array<std::array<std::uint8_t, 4>, 256> cache4_{};
  std::array<std::array<std::uint8_t, 8>, 256> cache8_{};
};

// ---- Cinepak ------------------------------------------------------------------------------------------

class CinepakDecoder final : public SimpleDecoder {
 protected:
  Result<VideoFrame> decode(const Packet& packet) override {
    util::ByteReader r(packet.bytes);
    const std::uint8_t flags = r.u8();
    r.skip(3);  // frame size
    const std::uint32_t w = r.u16be();
    const std::uint32_t h = r.u16be();
    const std::uint32_t strips = r.u16be();
    if (!r.ok()) return fail(Status::Decode, "truncated Cinepak frame header");
    const std::uint32_t width = w != 0 ? w : params_.width;
    const std::uint32_t height = h != 0 ? h : params_.height;
    CVT_TRY(check_dimensions(width, height));
    canvas_.ensure(width, height);
    if (strips > 32) return fail(Status::Decode, "too many Cinepak strips ({})", strips);
    if (strip_books_.size() < strips) strip_books_.resize(strips);
    std::uint32_t y0 = 0;
    for (std::uint32_t s = 0; s < strips; ++s) {
      const std::uint32_t id = r.u16be();
      const std::uint32_t size = r.u16be();
      r.skip(2);  // top (relative)
      r.skip(2);  // left
      const std::uint32_t bottom = r.u16be();
      r.skip(2);  // right
      if (!r.ok() || size < 12 || (id != 0x1000 && id != 0x1100)) return fail(Status::Decode, "damaged Cinepak strip header");
      // Strips inherit the codebooks of the previous strip unless they are independent.
      if (s > 0 && (flags & 1) == 0) strip_books_[s] = strip_books_[s - 1];
      const std::uint32_t strip_height = bottom;  // relative bottom = strip height in the wild
      util::ByteReader chunk_reader(ByteView(packet.bytes).subspan(r.position(), std::min<std::size_t>(size - 12, r.remaining())));
      r.skip(size - 12);
      CVT_TRY(decode_strip(chunk_reader, strip_books_[s], y0, strip_height));
      y0 += strip_height;
    }
    return canvas_.frame;
  }

 private:
  struct Entry {
    std::array<Rgba, 4> px{};
  };
  struct Books {
    std::array<Entry, 256> v1{};
    std::array<Entry, 256> v4{};
  };

  static Entry make_entry(const std::uint8_t* y, int u, int v, bool grey) {
    Entry e;
    for (int k = 0; k < 4; ++k) {
      const int yy = y[k];
      if (grey) {
        e.px[static_cast<std::size_t>(k)] = {static_cast<std::uint8_t>(yy), static_cast<std::uint8_t>(yy), static_cast<std::uint8_t>(yy), 255};
      } else {
        e.px[static_cast<std::size_t>(k)] = {clamp_u8(yy + 2 * v), clamp_u8(yy - u / 2 - v), clamp_u8(yy + 2 * u), 255};
      }
    }
    return e;
  }

  Result<void> decode_codebook(util::ByteReader& r, std::array<Entry, 256>& book, std::uint32_t id, std::uint32_t size) {
    const bool grey = (id & 0x0400) != 0;
    const bool partial = (id & 0x0100) != 0;
    const std::size_t entry_size = grey ? 4 : 6;
    util::ByteReader c(r.bytes(size));
    std::uint32_t mask = 0;
    int bits = 0;
    for (std::size_t i = 0; i < 256 && c.remaining() >= entry_size; ++i) {
      if (partial) {
        if (bits == 0) {
          if (c.remaining() < 4) break;
          mask = c.u32be();
          bits = 32;
        }
        const bool update = (mask & 0x80000000u) != 0;
        mask <<= 1;
        --bits;
        if (!update) continue;
        if (c.remaining() < entry_size) break;
      }
      std::uint8_t y[4];
      for (auto& v : y) v = c.u8();
      int u = 0;
      int v = 0;
      if (!grey) {
        u = static_cast<std::int8_t>(c.u8());
        v = static_cast<std::int8_t>(c.u8());
      }
      book[i] = make_entry(y, u, v, grey);
    }
    return {};
  }

  void put_v1(const Entry& e, std::uint32_t x, std::uint32_t y) {
    for (std::uint32_t j = 0; j < 4; ++j) {
      for (std::uint32_t i = 0; i < 4; ++i) canvas_.set(x + i, y + j, e.px[(j / 2) * 2 + i / 2]);
    }
  }
  void put_v4(const Entry* e[4], std::uint32_t x, std::uint32_t y) {
    for (std::uint32_t q = 0; q < 4; ++q) {
      const std::uint32_t qx = x + (q & 1) * 2;
      const std::uint32_t qy = y + (q >> 1) * 2;
      for (std::uint32_t k = 0; k < 4; ++k) canvas_.set(qx + (k & 1), qy + (k >> 1), e[q]->px[k]);
    }
  }

  Result<void> decode_strip(util::ByteReader& r, Books& books, std::uint32_t top, std::uint32_t height) {
    while (r.remaining() >= 4) {
      const std::uint32_t id = r.u16be();
      const std::uint32_t size = r.u16be();
      if (size < 4 || size - 4 > r.remaining()) return fail(Status::Decode, "damaged Cinepak chunk");
      const std::uint32_t body = size - 4;
      switch (id & 0xFF00) {
        case 0x2000:
        case 0x2100:
        case 0x2400:
        case 0x2500: CVT_TRY(decode_codebook(r, books.v4, id, body)); break;
        case 0x2200:
        case 0x2300:
        case 0x2600:
        case 0x2700: CVT_TRY(decode_codebook(r, books.v1, id, body)); break;
        case 0x3000:
        case 0x3100:
        case 0x3200: {
          util::ByteReader d(r.bytes(body));
          decode_vectors(d, books, id, top, height);
          break;
        }
        default: r.skip(body); break;
      }
    }
    return {};
  }

  void decode_vectors(util::ByteReader& d, const Books& books, std::uint32_t id, std::uint32_t top, std::uint32_t height) {
    std::uint32_t mask = 0;
    int bits = 0;
    const auto flag = [&]() -> bool {
      if (bits == 0) {
        mask = d.u32be();
        bits = 32;
      }
      const bool v = (mask & 0x80000000u) != 0;
      mask <<= 1;
      --bits;
      return v;
    };
    const std::uint32_t width = canvas_.frame.width;
    for (std::uint32_t y = top; y < top + height && y < canvas_.frame.height; y += 4) {
      for (std::uint32_t x = 0; x < width; x += 4) {
        if (!d.ok() || (d.at_end() && bits == 0)) return;
        if (id == 0x3200) {
          put_v1(books.v1[d.u8()], x, y);
          continue;
        }
        if (id == 0x3100 && !flag()) continue;  // unchanged block
        if (!d.ok()) return;
        if (flag()) {
          const Entry* e[4] = {&books.v4[d.u8()], &books.v4[d.u8()], &books.v4[d.u8()], &books.v4[d.u8()]};
          put_v4(e, x, y);
        } else {
          put_v1(books.v1[d.u8()], x, y);
        }
      }
    }
  }

  Canvas canvas_;
  std::vector<Books> strip_books_;
};

}  // namespace

void add_simple_codecs(std::vector<CodecEntry>& out) {
  out.push_back({id::qtrle, [] { return std::make_unique<QtrleDecoder>(); }, [] { return std::make_unique<QtrleEncoder>(); }});
  out.push_back({id::msrle, [] { return std::make_unique<MsrleDecoder>(); }, {}});
  out.push_back({id::msvideo1, [] { return std::make_unique<MsVideo1Decoder>(); }, {}});
  out.push_back({id::rpza, [] { return std::make_unique<RpzaDecoder>(); }, {}});
  out.push_back({id::smc, [] { return std::make_unique<SmcDecoder>(); }, {}});
  out.push_back({id::cinepak, [] { return std::make_unique<CinepakDecoder>(); }, {}});
}

}  // namespace cvt::legacy
