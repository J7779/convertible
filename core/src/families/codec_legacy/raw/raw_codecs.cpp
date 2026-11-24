// Convertible core - codec_legacy family: uncompressed video variants.
//
//   uyvy   packed 4:2:2, U0 Y0 V0 Y1 per pixel pair (QuickTime '2vuy', AVI 'UYVY') -> I422
//   yuy2   packed 4:2:2, Y0 U0 Y1 V0 (AVI 'YUY2', QuickTime 'yuvs')              -> I422
//   v210   packed 4:2:2 10-bit, three samples per little-endian 32-bit word, rows
//          padded to multiples of 48 pixels (128 bytes)                           -> I422P10
//   bgr24  B G R bytes, rows padded to 4 bytes, bottom-up (AVI BI_RGB with a
//          positive height); `bits_per_sample` 24. Containers that store top-down
//          rows (negative height) pass extradata "top-down"                    -> RGBA8
//   r210   big-endian 32-bit words 00 RRRRRRRRRR GGGGGGGGGG BBBBBBBBBB, rows padded
//          to multiples of 64 pixels                                             -> RGBA8 (10 -> 8 bit, rounded)
//
// Each packet holds exactly one picture; a short packet is a decoding error.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <cstring>

#include "families/codec_legacy/codecs.hpp"
#include "families/codec_legacy/common/codec_base.hpp"

namespace cvt::legacy {

namespace {

enum class RawKind { Uyvy, Yuy2, V210, Bgr24, R210 };

[[nodiscard]] std::uint64_t raw_row_bytes(RawKind kind, std::uint32_t width) {
  const std::uint64_t w = width;
  switch (kind) {
    case RawKind::Uyvy:
    case RawKind::Yuy2: return (w + 1) / 2 * 4;
    case RawKind::V210: return (w + 47) / 48 * 128;
    case RawKind::Bgr24: return (w * 3 + 3) / 4 * 4;
    case RawKind::R210: return (w + 63) / 64 * 256;
  }
  return 0;
}

[[nodiscard]] std::uint8_t ten_to_eight(std::uint32_t v) { return static_cast<std::uint8_t>((v * 255 + 511) / 1023); }

class RawDecoder final : public SimpleDecoder {
 public:
  explicit RawDecoder(RawKind kind) : kind_(kind) {}

 protected:
  Result<void> on_configure() override {
    if (params_.width == 0 || params_.height == 0) {
      return fail(Status::Decode, "uncompressed video needs the picture size from the container");
    }
    top_down_ = to_string(ByteView(params_.extradata)) == "top-down";
    return {};
  }

  Result<VideoFrame> decode(const Packet& packet) override {
    const std::uint32_t w = params_.width;
    const std::uint32_t h = params_.height;
    const std::uint64_t row = raw_row_bytes(kind_, w);
    if (packet.bytes.size() < row * h) {
      return fail(Status::Decode, "uncompressed picture is truncated ({} of {} bytes)", packet.bytes.size(), row * h);
    }
    const auto* src = reinterpret_cast<const std::uint8_t*>(packet.bytes.data());
    VideoFrame f;
    switch (kind_) {
      case RawKind::Uyvy:
      case RawKind::Yuy2: {
        f = VideoFrame::allocate(PixelFormat::I422, w, h);
        const bool uyvy = kind_ == RawKind::Uyvy;
        for (std::uint32_t y = 0; y < h; ++y) {
          const std::uint8_t* s = src + y * row;
          std::uint8_t* luma = f.planes[0].data.data() + y * f.planes[0].stride;
          std::uint8_t* cb = f.planes[1].data.data() + y * f.planes[1].stride;
          std::uint8_t* cr = f.planes[2].data.data() + y * f.planes[2].stride;
          for (std::uint32_t x = 0; x < (w + 1) / 2; ++x) {
            const std::uint8_t* q = s + x * 4;
            cb[x] = uyvy ? q[0] : q[1];
            cr[x] = uyvy ? q[2] : q[3];
            luma[2 * x] = uyvy ? q[1] : q[0];
            if (2 * x + 1 < w) luma[2 * x + 1] = uyvy ? q[3] : q[2];
          }
        }
        f.color = {ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Left};
        if (h >= 720) f.color.matrix = ir::ColorMatrix::BT709;
        break;
      }
      case RawKind::V210: {
        f = VideoFrame::allocate(PixelFormat::I422P10, w, h);
        for (std::uint32_t y = 0; y < h; ++y) {
          const std::uint8_t* s = src + y * row;
          auto* luma = f.planes[0].data.data() + y * f.planes[0].stride;
          auto* cb = f.planes[1].data.data() + y * f.planes[1].stride;
          auto* cr = f.planes[2].data.data() + y * f.planes[2].stride;
          const auto put = [](std::uint8_t* plane, std::uint32_t index, std::uint32_t v) {
            plane[index * 2] = static_cast<std::uint8_t>(v & 0xFF);
            plane[index * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
          };
          // Sample order of the packed stream: Cb Y Cr Y Cb Y Cr Y ...
          std::uint32_t yi = 0;
          std::uint32_t ci = 0;
          int phase = 0;  // 0 Cb, 1 Y, 2 Cr, 3 Y
          const std::uint32_t total = ((w + 1) / 2) * 4;
          std::uint32_t produced = 0;
          for (std::uint32_t word = 0; produced < total; ++word) {
            const std::uint32_t v = static_cast<std::uint32_t>(s[word * 4]) | static_cast<std::uint32_t>(s[word * 4 + 1]) << 8 |
                                    static_cast<std::uint32_t>(s[word * 4 + 2]) << 16 |
                                    static_cast<std::uint32_t>(s[word * 4 + 3]) << 24;
            for (int k = 0; k < 3 && produced < total; ++k, ++produced) {
              const std::uint32_t sample = (v >> (10 * k)) & 0x3FF;
              switch (phase) {
                case 0: put(cb, ci, sample); break;
                case 2: put(cr, ci, sample); ++ci; break;
                default:
                  if (yi < w) put(luma, yi, sample);
                  ++yi;
                  break;
              }
              phase = (phase + 1) & 3;
            }
          }
        }
        f.color = {h >= 720 ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Left};
        break;
      }
      case RawKind::Bgr24:
      case RawKind::R210: {
        f = VideoFrame::allocate(PixelFormat::RGBA8, w, h);
        for (std::uint32_t y = 0; y < h; ++y) {
          const std::uint32_t source_row = kind_ == RawKind::Bgr24 && !top_down_ ? h - 1 - y : y;
          const std::uint8_t* s = src + source_row * row;
          std::uint8_t* d = f.planes[0].data.data() + y * f.planes[0].stride;
          for (std::uint32_t x = 0; x < w; ++x) {
            if (kind_ == RawKind::Bgr24) {
              d[x * 4] = s[x * 3 + 2];
              d[x * 4 + 1] = s[x * 3 + 1];
              d[x * 4 + 2] = s[x * 3];
            } else {
              const std::uint32_t v = static_cast<std::uint32_t>(s[x * 4]) << 24 | static_cast<std::uint32_t>(s[x * 4 + 1]) << 16 |
                                      static_cast<std::uint32_t>(s[x * 4 + 2]) << 8 | s[x * 4 + 3];
              d[x * 4] = ten_to_eight((v >> 20) & 0x3FF);
              d[x * 4 + 1] = ten_to_eight((v >> 10) & 0x3FF);
              d[x * 4 + 2] = ten_to_eight(v & 0x3FF);
            }
            d[x * 4 + 3] = 255;
          }
        }
        break;
      }
    }
    return f;
  }

 private:
  RawKind kind_;
  bool top_down_ = false;
};

class RawEncoder final : public SimpleEncoder {
 public:
  explicit RawEncoder(RawKind kind) : kind_(kind) {}

 protected:
  Result<Bytes> encode(const VideoFrame& frame, bool& keyframe) override {
    keyframe = true;
    const std::uint32_t w = params_.width;
    const std::uint32_t h = params_.height;
    const std::uint64_t row = raw_row_bytes(kind_, w);
    Bytes out(static_cast<std::size_t>(row * h));
    auto* dst = reinterpret_cast<std::uint8_t*>(out.data());
    ColorInfo color = params_.color;
    color.range = ir::ColorRange::Limited;
    color.siting = ir::ChromaSiting::Left;
    switch (kind_) {
      case RawKind::Uyvy:
      case RawKind::Yuy2: {
        CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, PixelFormat::I422, color));
        const bool uyvy = kind_ == RawKind::Uyvy;
        for (std::uint32_t y = 0; y < h; ++y) {
          std::uint8_t* d = dst + y * row;
          const std::uint8_t* luma = f.planes[0].data.data() + y * f.planes[0].stride;
          const std::uint8_t* cb = f.planes[1].data.data() + y * f.planes[1].stride;
          const std::uint8_t* cr = f.planes[2].data.data() + y * f.planes[2].stride;
          for (std::uint32_t x = 0; x < (w + 1) / 2; ++x) {
            const std::uint8_t y0 = luma[2 * x];
            const std::uint8_t y1 = 2 * x + 1 < w ? luma[2 * x + 1] : y0;
            std::uint8_t* q = d + x * 4;
            if (uyvy) {
              q[0] = cb[x], q[1] = y0, q[2] = cr[x], q[3] = y1;
            } else {
              q[0] = y0, q[1] = cb[x], q[2] = y1, q[3] = cr[x];
            }
          }
        }
        break;
      }
      case RawKind::V210: {
        CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, PixelFormat::I422P10, color));
        for (std::uint32_t y = 0; y < h; ++y) {
          std::uint8_t* d = dst + y * row;
          const auto get = [&](std::size_t plane, std::uint32_t index) -> std::uint32_t {
            const std::uint8_t* p = f.planes[plane].data.data() + y * f.planes[plane].stride;
            const std::uint32_t limit = static_cast<std::uint32_t>(ir::plane_width(PixelFormat::I422P10, plane, w));
            if (index >= limit) index = limit - 1;
            return static_cast<std::uint32_t>(p[index * 2] | p[index * 2 + 1] << 8) & 0x3FF;
          };
          const std::uint32_t groups = (w + 47) / 48 * 48 / 2;  // pixel pairs incl. padding
          std::uint32_t samples[4];
          std::uint32_t word = 0;
          int filled = 0;
          std::uint32_t at = 0;
          for (std::uint32_t pair = 0; pair < groups; ++pair) {
            samples[0] = get(1, pair);
            samples[1] = get(0, pair * 2);
            samples[2] = get(2, pair);
            samples[3] = get(0, pair * 2 + 1);
            for (const std::uint32_t s : samples) {
              word |= s << (10 * filled);
              if (++filled == 3) {
                for (int k = 0; k < 4; ++k) d[at++] = static_cast<std::uint8_t>(word >> (8 * k));
                word = 0;
                filled = 0;
              }
            }
          }
        }
        break;
      }
      case RawKind::Bgr24:
      case RawKind::R210: {
        CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, PixelFormat::RGBA8, color));
        for (std::uint32_t y = 0; y < h; ++y) {
          const std::uint8_t* s = f.planes[0].data.data() + y * f.planes[0].stride;
          std::uint8_t* d = dst + (kind_ == RawKind::Bgr24 ? h - 1 - y : y) * row;
          for (std::uint32_t x = 0; x < w; ++x) {
            if (kind_ == RawKind::Bgr24) {
              d[x * 3] = s[x * 4 + 2];
              d[x * 3 + 1] = s[x * 4 + 1];
              d[x * 3 + 2] = s[x * 4];
            } else {
              const auto ten = [](std::uint8_t v) { return static_cast<std::uint32_t>(v) * 4 + v / 64; };
              const std::uint32_t v = ten(s[x * 4]) << 20 | ten(s[x * 4 + 1]) << 10 | ten(s[x * 4 + 2]);
              d[x * 4] = static_cast<std::uint8_t>(v >> 24);
              d[x * 4 + 1] = static_cast<std::uint8_t>(v >> 16);
              d[x * 4 + 2] = static_cast<std::uint8_t>(v >> 8);
              d[x * 4 + 3] = static_cast<std::uint8_t>(v);
            }
          }
        }
        break;
      }
    }
    return out;
  }

 private:
  RawKind kind_;
};

void add(std::vector<CodecEntry>& out, std::string_view codec_id, RawKind kind) {
  out.push_back({codec_id, [kind] { return std::make_unique<RawDecoder>(kind); },
                 [kind] { return std::make_unique<RawEncoder>(kind); }});
}

}  // namespace

void add_raw_codecs(std::vector<CodecEntry>& out) {
  add(out, id::uyvy, RawKind::Uyvy);
  add(out, id::yuy2, RawKind::Yuy2);
  add(out, id::v210, RawKind::V210);
  add(out, id::bgr24, RawKind::Bgr24);
  add(out, id::r210, RawKind::R210);
}

}  // namespace cvt::legacy
