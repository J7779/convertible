// Convertible core - codec_vpx family: VP8 and VP9 decoders (libvpx).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <string>

#include "util/bytes.hpp"
#include "codec/codec.hpp"
#include "families/codec_vpx/vpx_bitstream.hpp"
#include "families/codec_vpx/vpx_codecs.hpp"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

namespace cvt::vpx {

namespace {

using codec::CodecParams;
using ir::ColorInfo;
using ir::ColorMatrix;
using ir::ColorRange;
using ir::PixelFormat;
using ir::Plane;
using ir::VideoFrame;

/// `ticks / timescale` seconds in milliseconds, rounded to nearest, without
/// intermediate overflow.
std::int64_t ticks_to_ms(std::int64_t ticks, std::uint32_t timescale) {
  const std::int64_t scale = timescale == 0 ? 1000 : timescale;
  std::int64_t whole = ticks / scale;
  std::int64_t rest = ticks % scale;
  if (rest < 0) {
    rest += scale;
    --whole;
  }
  constexpr std::int64_t limit = std::numeric_limits<std::int64_t>::max() / 1000 - 1;
  whole = std::clamp(whole, -limit, limit);
  return whole * 1000 + (rest * 1000 + scale / 2) / scale;
}

std::string codec_error(vpx_codec_ctx_t& context) {
  std::string message = vpx_codec_error(&context);
  if (const char* detail = vpx_codec_error_detail(&context); detail != nullptr && *detail != '\0') {
    message += ": ";
    message += detail;
  }
  return message;
}

/// Matrix used for a VP9 colour space code. "Unknown" follows the common
/// player convention: BT.601 below 720 lines, BT.709 from 720 lines up.
ColorMatrix vp9_matrix(vpx_color_space_t space, std::uint32_t height) {
  switch (space) {
    case VPX_CS_BT_709:
    case VPX_CS_SMPTE_240: return ColorMatrix::BT709;
    case VPX_CS_BT_2020: return ColorMatrix::BT2020;
    case VPX_CS_UNKNOWN: return height >= 720 ? ColorMatrix::BT709 : ColorMatrix::BT601;
    default: return ColorMatrix::BT601;
  }
}

/// Reads one sample of a libvpx plane as an integer of the output depth.
struct SampleReader {
  bool high = false;  ///< 16-bit storage
  int shift = 0;      ///< right shift to reach the output depth (12 -> 10 bit)

  [[nodiscard]] std::uint32_t at(const std::uint8_t* row, std::size_t x) const noexcept {
    if (!high) return row[x];
    std::uint16_t value = 0;
    ::cvt::util::copy_bytes(&value, row + x * 2, 2);  // libvpx stores host-order (little-endian) words
    if (shift == 0) return value;
    const std::uint32_t rounded = (std::uint32_t{value} + (1u << (shift - 1))) >> shift;
    return std::min<std::uint32_t>(rounded, 1023u);
  }
};

void put_sample(std::uint8_t* row, std::size_t x, std::uint32_t value, bool wide) {
  if (wide) {
    row[x * 2] = static_cast<std::uint8_t>(value & 0xFFu);
    row[x * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
  } else {
    row[x] = static_cast<std::uint8_t>(value);
  }
}

Result<VideoFrame> image_to_frame(const vpx_image_t& image, Codec codec, const std::optional<ColorInfo>& vp8_color) {
  const std::uint32_t width = image.d_w;
  const std::uint32_t height = image.d_h;
  if (width == 0 || height == 0) return fail(Status::Decode, "decoded picture has no pixels");
  const bool high = (image.fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0;
  const int depth = high ? static_cast<int>(image.bit_depth) : 8;
  if (depth != 8 && depth != 10 && depth != 12) return fail(Status::Decode, "unsupported bit depth {}", depth);
  const bool out_wide = depth > 8;
  SampleReader reader{high, depth == 12 ? 2 : 0};
  const int sx = static_cast<int>(image.x_chroma_shift);
  const int sy = static_cast<int>(image.y_chroma_shift);
  const std::size_t chroma_cols = (width + static_cast<std::uint32_t>(sx)) >> sx;

  ColorInfo color;
  if (codec == Codec::Vp8) {
    color = vp8_color.value_or(ColorInfo{ColorMatrix::BT601, ColorRange::Limited, ir::ChromaSiting::Left});
  } else {
    color.matrix = vp9_matrix(image.cs, height);
    color.range = image.range == VPX_CR_FULL_RANGE ? ColorRange::Full : ColorRange::Limited;
    color.siting = ir::ChromaSiting::Left;
  }

  if (codec == Codec::Vp9 && image.cs == VPX_CS_SRGB) {
    // 4:4:4 RGB: the planes hold G, B and R.
    if (sx != 0 || sy != 0) return fail(Status::Decode, "subsampled RGB VP9 is not supported");
    VideoFrame frame = VideoFrame::allocate(PixelFormat::RGBA8, width, height);
    frame.color = color;
    const int down = depth - 8;
    for (std::uint32_t y = 0; y < height; ++y) {
      const std::uint8_t* g = image.planes[0] + static_cast<std::ptrdiff_t>(y) * image.stride[0];
      const std::uint8_t* b = image.planes[1] + static_cast<std::ptrdiff_t>(y) * image.stride[1];
      const std::uint8_t* r = image.planes[2] + static_cast<std::ptrdiff_t>(y) * image.stride[2];
      std::uint8_t* out = frame.planes[0].data.data() + std::size_t{y} * frame.planes[0].stride;
      const SampleReader raw{high, 0};
      for (std::uint32_t x = 0; x < width; ++x) {
        const auto scale = [down](std::uint32_t v) {
          if (down == 0) return static_cast<std::uint8_t>(v);
          return static_cast<std::uint8_t>(std::min<std::uint32_t>((v + (1u << (down - 1))) >> down, 255u));
        };
        out[x * 4] = scale(raw.at(r, x));
        out[x * 4 + 1] = scale(raw.at(g, x));
        out[x * 4 + 2] = scale(raw.at(b, x));
        out[x * 4 + 3] = 255;
      }
    }
    return frame;
  }

  PixelFormat format = PixelFormat::I420;
  if (sx == 1 && sy == 1) {
    format = out_wide ? PixelFormat::I420P10 : PixelFormat::I420;
  } else if (sx == 1 && sy == 0) {
    format = out_wide ? PixelFormat::I422P10 : PixelFormat::I422;
  } else if (sx == 0) {
    format = out_wide ? PixelFormat::I444P10 : PixelFormat::I444;  // 4:4:0 is widened to 4:4:4
  } else {
    return fail(Status::Decode, "unsupported chroma layout");
  }
  VideoFrame frame = VideoFrame::allocate(format, width, height);
  frame.color = color;
  for (std::size_t p = 0; p < 3; ++p) {
    const std::size_t rows = ir::plane_height(format, p, height);
    const std::size_t cols = ir::plane_width(format, p, width);
    // 4:4:0 -> 4:4:4 repeats each chroma row; every other layout copies rows 1:1.
    const int row_shift = p == 0 ? 0 : sy - ir::chroma_shift_y(format);
    if (p > 0 && cols != chroma_cols) return fail(Status::Internal, "chroma plane width mismatch");
    Plane& plane = frame.planes[p];
    for (std::size_t y = 0; y < rows; ++y) {
      const std::size_t source_row = y >> row_shift;
      const std::uint8_t* in = image.planes[p] + static_cast<std::ptrdiff_t>(source_row) * image.stride[p];
      std::uint8_t* out = plane.data.data() + y * plane.stride;
      if (!high && !out_wide) {
        ::cvt::util::copy_bytes(out, in, cols);
      } else if (high && out_wide && reader.shift == 0) {
        ::cvt::util::copy_bytes(out, in, cols * 2);  // host order is little-endian (see the libvpx configuration)
      } else {
        for (std::size_t x = 0; x < cols; ++x) put_sample(out, x, reader.at(in, x), out_wide);
      }
    }
  }
  return frame;
}

struct PendingFrame {
  VideoFrame frame;
  std::int64_t pts = 0;       ///< ticks
  std::int64_t duration = 0;  ///< ticks; 0 = unknown
};

class VpxDecoder final : public codec::VideoDecoder {
 public:
  explicit VpxDecoder(Codec codec) : codec_(codec) {}
  ~VpxDecoder() override {
    if (initialised_) vpx_codec_destroy(&context_);
  }
  VpxDecoder(const VpxDecoder&) = delete;
  VpxDecoder& operator=(const VpxDecoder&) = delete;

  Result<void> configure(const CodecParams& params) override {
    if (initialised_) return fail(Status::Internal, "decoder configured twice");
    timescale_ = params.timescale == 0 ? 1000 : params.timescale;
    if (params.max_pixels > 0) max_pixels_ = std::min(max_pixels_, params.max_pixels);
    if (codec_ == Codec::Vp8 && !params.extradata.empty()) {
      // Optional vpcC: the only place a VP8 stream can state its colours.
      if (const auto config = parse_vpcc(params.extradata)) {
        ColorInfo color{ColorMatrix::BT601, config->full_range ? ColorRange::Full : ColorRange::Limited,
                        ir::ChromaSiting::Left};
        if (config->matrix_coefficients == 1) color.matrix = ColorMatrix::BT709;
        if (config->matrix_coefficients == 9) color.matrix = ColorMatrix::BT2020;
        if (config->chroma_subsampling == 1) color.siting = ir::ChromaSiting::TopLeft;
        vp8_color_ = color;
      }
    }
    vpx_codec_dec_cfg_t config{};
    config.threads = 1;
    config.w = params.width;
    config.h = params.height;
    vpx_codec_iface_t* iface = codec_ == Codec::Vp8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx();
    if (vpx_codec_dec_init(&context_, iface, &config, 0) != VPX_CODEC_OK) {
      return fail(Status::Internal, "cannot start the {} decoder: {}", name(), codec_error(context_));
    }
    initialised_ = true;
    return {};
  }

  Result<void> send_packet(const ir::Packet& packet) override {
    CVT_TRY(usable());
    if (flushed_) return fail(Status::Internal, "packet sent after flush");
    if (packet.bytes.empty()) return {};  // a dropped frame: nothing to show
    if (packet.bytes.size() > std::numeric_limits<unsigned int>::max()) {
      return poison(fail(Status::Limit, "{} packet of {} bytes is too large", name(), packet.bytes.size()));
    }
    if (auto checked = check_headers(packet.bytes); !checked) return poison(checked);
    const auto* data = reinterpret_cast<const std::uint8_t*>(packet.bytes.data());
    if (vpx_codec_decode(&context_, data, static_cast<unsigned int>(packet.bytes.size()), nullptr, 0) !=
        VPX_CODEC_OK) {
      return poison(fail(Status::Decode, "{} frame cannot be decoded: {}", name(), codec_error(context_)));
    }
    return collect(packet.pts, packet.duration);
  }

  Result<std::optional<VideoFrame>> receive_frame() override {
    CVT_TRY(usable());
    if (ready_.empty()) return std::optional<VideoFrame>{};
    PendingFrame next = std::move(ready_.front());
    ready_.pop_front();
    next.frame.pts_ms = ticks_to_ms(next.pts, timescale_);
    next.frame.duration_ms = ticks_to_ms(next.pts + next.duration, timescale_) - next.frame.pts_ms;
    return std::optional<VideoFrame>{std::move(next.frame)};
  }

  Result<void> flush() override {
    CVT_TRY(usable());
    if (flushed_) return {};
    flushed_ = true;
    if (vpx_codec_decode(&context_, nullptr, 0, nullptr, 0) != VPX_CODEC_OK) {
      return poison(fail(Status::Decode, "{} decoder cannot finish: {}", name(), codec_error(context_)));
    }
    CVT_TRY(collect(last_pts_, 0));
    if (held_) {
      held_->duration = last_duration_;
      ready_.push_back(std::move(*held_));
      held_.reset();
    }
    return {};
  }

 private:
  [[nodiscard]] std::string_view name() const { return codec_ == Codec::Vp8 ? "VP8" : "VP9"; }

  Result<void> usable() const {
    if (!initialised_) return fail(Status::Internal, "decoder used before configure");
    if (failed_) return fail(Status::Internal, "decoder used after an error");
    return {};
  }
  template <class T>
  T poison(T result) {
    failed_ = true;
    return result;
  }

  /// Refuse frames whose declared size exceeds the limit, before libvpx
  /// allocates their buffers.
  Result<void> check_headers(ByteView packet) {
    const auto check_size = [this](std::uint32_t width, std::uint32_t height) -> Result<void> {
      if (std::uint64_t{width} * height > max_pixels_) {
        return fail(Status::Limit, "{} frame of {}x{} pixels exceeds the limit of {} pixels", name(), width, height,
                    max_pixels_);
      }
      return {};
    };
    if (codec_ == Codec::Vp8) {
      CVT_TRY_ASSIGN(const FrameHeader header, parse_vp8_header(packet));
      if (header.keyframe) CVT_TRY(check_size(header.width, header.height));
      return {};
    }
    CVT_TRY_ASSIGN(const std::vector<ByteView> frames, split_vp9_superframe(packet));
    for (const ByteView frame : frames) {
      CVT_TRY_ASSIGN(const FrameHeader header, parse_vp9_header(frame, &refs_));
      CVT_TRY(check_size(header.width, header.height));
    }
    return {};
  }

  /// Take every picture libvpx has ready; each belongs to the packet just sent.
  Result<void> collect(std::int64_t pts, std::int64_t duration) {
    vpx_codec_iter_t iterator = nullptr;
    while (const vpx_image_t* image = vpx_codec_get_frame(&context_, &iterator)) {
      auto frame = image_to_frame(*image, codec_, vp8_color_);
      if (!frame) return poison(std::unexpected(frame.error()));
      PendingFrame pending{std::move(*frame), pts, duration > 0 ? duration : 0};
      if (held_) {
        // The previous frame had no duration of its own: it lasts until this one.
        const std::int64_t gap = pts - held_->pts;
        held_->duration = gap > 0 ? gap : last_duration_;
        if (gap > 0) last_duration_ = gap;
        ready_.push_back(std::move(*held_));
        held_.reset();
      }
      if (pending.duration > 0) {
        last_duration_ = pending.duration;
        ready_.push_back(std::move(pending));
      } else {
        held_ = std::move(pending);
      }
    }
    last_pts_ = pts;
    return {};
  }

  Codec codec_;
  vpx_codec_ctx_t context_{};
  bool initialised_ = false;
  bool failed_ = false;
  bool flushed_ = false;
  std::uint32_t timescale_ = 1000;
  std::uint64_t max_pixels_ = max_frame_pixels;  ///< lowered by CodecParams::max_pixels
  std::optional<ColorInfo> vp8_color_;
  Vp9RefSizes refs_;
  std::deque<PendingFrame> ready_;
  std::optional<PendingFrame> held_;
  std::int64_t last_pts_ = 0;
  std::int64_t last_duration_ = 0;
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_decoder(Codec codec) { return std::make_unique<VpxDecoder>(codec); }

std::string_view libvpx_version() noexcept { return vpx_codec_version_str(); }

}  // namespace cvt::vpx
