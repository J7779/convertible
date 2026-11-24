// Convertible core - codec_vpx family: VP8 and VP9 encoders (libvpx).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <string>

#include "codec/codec.hpp"
#include "codec/colorspace.hpp"
#include "families/codec_vpx/vpx_bitstream.hpp"
#include "families/codec_vpx/vpx_codecs.hpp"
#include "vpx/vp8cx.h"
#include "vpx/vpx_encoder.h"

namespace cvt::vpx {

namespace {

using codec::EncodePreset;
using codec::VideoEncodeParams;
using ir::ColorInfo;
using ir::ColorMatrix;
using ir::ColorRange;
using ir::Packet;
using ir::PixelFormat;
using ir::VideoFrame;

/// libvpx accepts at most this many pixels per side (16384 is its compiled
/// decode limit; keep encoder output decodable by ourselves).
constexpr std::uint32_t max_dimension = 16384;

std::string codec_error(vpx_codec_ctx_t& context) {
  std::string message = vpx_codec_error(&context);
  if (const char* detail = vpx_codec_error_detail(&context); detail != nullptr && *detail != '\0') {
    message += ": ";
    message += detail;
  }
  return message;
}

/// The frame layout the encoder works in, given what the caller will send.
struct Layout {
  PixelFormat format = PixelFormat::I420;
  vpx_img_fmt_t image_format = VPX_IMG_FMT_I420;
  unsigned int profile = 0;
  int bit_depth = 8;
};

Layout layout_for(Codec codec, PixelFormat input) {
  if (codec == Codec::Vp8) return {};
  switch (input) {
    case PixelFormat::I422: return {PixelFormat::I422, VPX_IMG_FMT_I422, 1, 8};
    case PixelFormat::I444: return {PixelFormat::I444, VPX_IMG_FMT_I444, 1, 8};
    case PixelFormat::I420P10:
    case PixelFormat::P010: return {PixelFormat::I420P10, VPX_IMG_FMT_I42016, 2, 10};
    case PixelFormat::I422P10: return {PixelFormat::I422P10, VPX_IMG_FMT_I42216, 3, 10};
    case PixelFormat::I444P10: return {PixelFormat::I444P10, VPX_IMG_FMT_I44416, 3, 10};
    default: return {};
  }
}

vpx_color_space_t vpx_space(ColorMatrix matrix) {
  switch (matrix) {
    case ColorMatrix::BT601: return VPX_CS_BT_601;
    case ColorMatrix::BT709: return VPX_CS_BT_709;
    case ColorMatrix::BT2020: return VPX_CS_BT_2020;
  }
  return VPX_CS_UNKNOWN;
}

/// `ms` milliseconds in ticks of 1/`timescale` s, rounded to nearest.
std::int64_t ms_to_ticks(std::int64_t ms, std::uint32_t timescale) {
  constexpr std::int64_t limit = std::numeric_limits<std::int64_t>::max() / 0x100000000ll;
  ms = std::clamp(ms, -limit, limit);
  const std::int64_t scaled = ms * static_cast<std::int64_t>(timescale);
  return scaled >= 0 ? (scaled + 500) / 1000 : -((-scaled + 500) / 1000);
}

class VpxEncoder final : public codec::VideoEncoder {
 public:
  explicit VpxEncoder(Codec codec) : codec_(codec) {}
  ~VpxEncoder() override {
    if (initialised_) vpx_codec_destroy(&context_);
    if (image_ != nullptr) vpx_img_free(image_);
  }
  VpxEncoder(const VpxEncoder&) = delete;
  VpxEncoder& operator=(const VpxEncoder&) = delete;

  Result<void> configure(const VideoEncodeParams& params) override {
    if (initialised_) return fail(Status::Internal, "encoder configured twice");
    if (params.width == 0 || params.height == 0) return fail(Status::InvalidArgument, "video has no pixels");
    if (params.width > max_dimension || params.height > max_dimension ||
        std::uint64_t{params.width} * params.height > max_frame_pixels) {
      return fail(Status::InvalidArgument, "{} cannot encode {}x{} pictures", name(), params.width, params.height);
    }
    if (params.timescale > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      return fail(Status::InvalidArgument, "timescale {} is too large", params.timescale);
    }
    width_ = params.width;
    height_ = params.height;
    timescale_ = params.timescale == 0 ? 90000 : params.timescale;
    layout_ = layout_for(codec_, params.pixel_format);
    color_ = params.color;
    if (codec_ == Codec::Vp8) {
      // VP8 has no colour signalling: decoders assume BT.601 limited range.
      color_ = ColorInfo{ColorMatrix::BT601, ColorRange::Limited, ir::ChromaSiting::Left};
    }
    const double fps = params.fps.value();
    if (fps > 0 && std::isfinite(fps)) {
      default_duration_ = std::max<std::int64_t>(
          1, static_cast<std::int64_t>(std::llround(static_cast<double>(timescale_) / fps)));
    } else {
      default_duration_ = std::max<std::int64_t>(1, timescale_ / 30);
    }

    const int quality = std::clamp(params.quality, 1, 100);
    lossless_ = !params.bitrate_kbps && quality >= 100;
    vpx_codec_iface_t* iface = codec_ == Codec::Vp8 ? vpx_codec_vp8_cx() : vpx_codec_vp9_cx();
    vpx_codec_enc_cfg_t config{};
    if (vpx_codec_enc_config_default(iface, &config, 0) != VPX_CODEC_OK) {
      return fail(Status::Internal, "cannot obtain the {} encoder defaults", name());
    }
    config.g_w = width_;
    config.g_h = height_;
    config.g_timebase.num = 1;
    config.g_timebase.den = static_cast<int>(timescale_);
    config.g_threads = 1;
    config.g_pass = VPX_RC_ONE_PASS;
    config.g_error_resilient = 0;
    config.g_profile = layout_.profile;
    config.g_bit_depth = layout_.bit_depth == 10 ? VPX_BITS_10 : VPX_BITS_8;
    config.g_input_bit_depth = static_cast<unsigned int>(layout_.bit_depth);
    // Single pass without look-ahead: libvpx builds hidden alternate
    // reference frames only from two-pass statistics (or in real-time mode),
    // so a lag would add latency and memory without improving the result.
    config.g_lag_in_frames = 0;
    config.kf_mode = VPX_KF_AUTO;
    config.kf_min_dist = 0;
    keyframe_interval_ = params.keyframe_interval != 0 ? params.keyframe_interval : default_keyframe_interval(fps);
    config.kf_max_dist = std::min<std::uint32_t>(keyframe_interval_, 9999);
    const int quantizer = quantizer_for_quality(quality);
    if (params.bitrate_kbps) {
      config.rc_end_usage = VPX_VBR;
      config.rc_target_bitrate = std::max<std::uint32_t>(*params.bitrate_kbps, 1);
      config.rc_min_quantizer = 0;
      config.rc_max_quantizer = 63;
    } else if (codec_ == Codec::Vp9) {
      config.rc_end_usage = VPX_Q;
      config.rc_min_quantizer = 0;
      config.rc_max_quantizer = lossless_ ? 0 : 63;
    } else {
      // VP8 has no constant-quality mode in one pass (VPX_Q behaves as VBR
      // and its rate control starts far too coarse), so the quantiser is
      // pinned: constrained quality with min = max = the chosen level and a
      // bitrate ceiling (about 2 bits per pixel) that never binds.
      config.rc_end_usage = VPX_CQ;
      config.rc_min_quantizer = static_cast<unsigned int>(quantizer);
      config.rc_max_quantizer = static_cast<unsigned int>(quantizer);
      const double rate = fps > 0 && std::isfinite(fps) ? fps : 30.0;
      const double kbps = static_cast<double>(width_) * height_ * rate * 2.0 / 1000.0;
      config.rc_target_bitrate = static_cast<unsigned int>(std::clamp(kbps, 1000.0, 1000000.0));
    }
    const vpx_codec_flags_t flags = layout_.bit_depth > 8 ? VPX_CODEC_USE_HIGHBITDEPTH : 0;
    if (vpx_codec_enc_init(&context_, iface, &config, flags) != VPX_CODEC_OK) {
      return fail(Status::InvalidArgument, "cannot start the {} encoder: {}", name(), codec_error(context_));
    }
    initialised_ = true;

    CVT_TRY(control(VP8E_SET_CPUUSED, cpu_used_for_preset(codec_, params.preset)));
    if (!params.bitrate_kbps) CVT_TRY(control(VP8E_SET_CQ_LEVEL, quantizer));
    if (codec_ == Codec::Vp9) {
      CVT_TRY(control(VP9E_SET_LOSSLESS, lossless_ ? 1 : 0));
      CVT_TRY(control(VP9E_SET_ROW_MT, 0));
      CVT_TRY(control(VP9E_SET_COLOR_SPACE, static_cast<int>(vpx_space(color_.matrix))));
      CVT_TRY(control(VP9E_SET_COLOR_RANGE,
                      static_cast<int>(color_.range == ColorRange::Full ? VPX_CR_FULL_RANGE : VPX_CR_STUDIO_RANGE)));
    } else {
      CVT_TRY(control(VP8E_SET_ENABLEAUTOALTREF, 0));
    }

    image_ = vpx_img_alloc(nullptr, layout_.image_format, width_, height_, 16);
    if (image_ == nullptr) return fail(Status::Internal, "out of memory for the {} input picture", name());
    image_->bit_depth = static_cast<unsigned int>(layout_.bit_depth);
    image_->cs = vpx_space(color_.matrix);
    image_->range = color_.range == ColorRange::Full ? VPX_CR_FULL_RANGE : VPX_CR_STUDIO_RANGE;

    VpcConfig vpcc;
    vpcc.profile = static_cast<std::uint8_t>(layout_.profile);
    vpcc.level = codec_ == Codec::Vp9 ? vp9_level(width_, height_, fps) : 0;
    vpcc.bit_depth = static_cast<std::uint8_t>(layout_.bit_depth);
    const int sx = ir::chroma_shift_x(layout_.format);
    const int sy = ir::chroma_shift_y(layout_.format);
    vpcc.chroma_subsampling =
        sx == 0 ? 3 : (sy == 0 ? 2 : (color_.siting == ir::ChromaSiting::TopLeft ? 1 : 0));
    vpcc.full_range = color_.range == ColorRange::Full;
    const H273Colour colour = h273_colour(color_.matrix, layout_.bit_depth);
    vpcc.colour_primaries = colour.primaries;
    vpcc.transfer_characteristics = colour.transfer;
    vpcc.matrix_coefficients = colour.matrix;
    extradata_ = make_vpcc(vpcc);
    return {};
  }

  Result<void> send_frame(const VideoFrame& frame) override {
    CVT_TRY(usable());
    if (flushed_) return fail(Status::Internal, "frame sent after flush");
    if (frame.width != width_ || frame.height != height_) {
      return fail(Status::InvalidArgument, "frame of {}x{} sent to a {}x{} {} encoder", frame.width, frame.height,
                  width_, height_, name());
    }
    if (!frame.valid()) return fail(Status::InvalidArgument, "invalid frame sent to the {} encoder", name());
    if (frame.format != layout_.format || (frame.color != color_ && ir::is_yuv(frame.format))) {
      CVT_TRY_ASSIGN(const VideoFrame converted, codec::convert_frame(frame, layout_.format, color_));
      copy_into_image(converted);
    } else {
      copy_into_image(frame);
    }

    std::int64_t pts = ms_to_ticks(frame.pts_ms, timescale_);
    if (frames_sent_ > 0 && pts <= last_pts_) pts = last_pts_ + 1;  // libvpx needs rising timestamps
    std::int64_t duration = frame.duration_ms > 0 ? ms_to_ticks(frame.duration_ms, timescale_) : default_duration_;
    duration = std::clamp<std::int64_t>(duration, 1, std::numeric_limits<std::uint32_t>::max());
    last_pts_ = pts;
    ++frames_sent_;
    if (vpx_codec_encode(&context_, image_, pts, static_cast<unsigned long>(duration), 0, VPX_DL_GOOD_QUALITY) !=
        VPX_CODEC_OK) {
      return poison(fail(Status::Encode, "{} encoding failed: {}", name(), codec_error(context_)));
    }
    drain();
    return {};
  }

  Result<std::optional<Packet>> receive_packet() override {
    CVT_TRY(usable());
    if (packets_.empty()) return std::optional<Packet>{};
    Packet packet = std::move(packets_.front());
    packets_.pop_front();
    return std::optional<Packet>{std::move(packet)};
  }

  [[nodiscard]] Bytes extradata() const override { return extradata_; }

  Result<void> flush() override {
    CVT_TRY(usable());
    if (flushed_) return {};
    flushed_ = true;
    // Each call releases what the look-ahead still holds; stop when dry.
    for (int round = 0; round < 1000; ++round) {
      if (vpx_codec_encode(&context_, nullptr, 0, 1, 0, VPX_DL_GOOD_QUALITY) != VPX_CODEC_OK) {
        return poison(fail(Status::Encode, "{} encoder cannot finish: {}", name(), codec_error(context_)));
      }
      if (!drain()) break;
    }
    return {};
  }

 private:
  [[nodiscard]] std::string_view name() const { return codec_ == Codec::Vp8 ? "VP8" : "VP9"; }

  Result<void> usable() const {
    if (!initialised_) return fail(Status::Internal, "encoder used before configure");
    if (failed_) return fail(Status::Internal, "encoder used after an error");
    return {};
  }
  template <class T>
  T poison(T result) {
    failed_ = true;
    return result;
  }

  template <class Id>
  Result<void> control(Id id, int value) {
    if (vpx_codec_control_(&context_, static_cast<int>(id), value) != VPX_CODEC_OK) {
      return poison(fail(Status::Internal, "{} encoder rejected setting {}: {}", name(), static_cast<int>(id),
                         codec_error(context_)));
    }
    return {};
  }

  /// Copy a frame already in the encoder's layout into the libvpx picture.
  void copy_into_image(const VideoFrame& frame) {
    const bool wide = layout_.bit_depth > 8;
    for (std::size_t p = 0; p < 3; ++p) {
      const std::size_t rows = ir::plane_height(layout_.format, p, height_);
      const std::size_t cols = ir::plane_width(layout_.format, p, width_);
      for (std::size_t y = 0; y < rows; ++y) {
        const std::uint8_t* in = frame.planes[p].data.data() + y * frame.planes[p].stride;
        std::uint8_t* out = image_->planes[p] + static_cast<std::ptrdiff_t>(y) * image_->stride[p];
        if (!wide) {
          std::memcpy(out, in, cols);
          continue;
        }
        for (std::size_t x = 0; x < cols; ++x) {
          const auto value = static_cast<std::uint16_t>(
              std::min<std::uint32_t>(in[x * 2] | (std::uint32_t{in[x * 2 + 1]} << 8), 1023u));
          std::memcpy(out + x * 2, &value, 2);  // libvpx works in host order (little-endian)
        }
      }
    }
  }

  /// Move every finished packet out of libvpx; true when there was any.
  bool drain() {
    bool any = false;
    vpx_codec_iter_t iterator = nullptr;
    while (const vpx_codec_cx_pkt_t* packet = vpx_codec_get_cx_data(&context_, &iterator)) {
      if (packet->kind != VPX_CODEC_CX_FRAME_PKT) continue;
      any = true;
      Packet out;
      const auto* data = static_cast<const std::byte*>(packet->data.frame.buf);
      out.bytes.assign(data, data + packet->data.frame.sz);
      out.pts = packet->data.frame.pts;
      out.dts = out.pts;
      out.duration = static_cast<std::int64_t>(packet->data.frame.duration);
      out.keyframe = (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
      packets_.push_back(std::move(out));
    }
    return any;
  }

  Codec codec_;
  vpx_codec_ctx_t context_{};
  vpx_image_t* image_ = nullptr;
  bool initialised_ = false;
  bool failed_ = false;
  bool flushed_ = false;
  bool lossless_ = false;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t timescale_ = 90000;
  std::uint32_t keyframe_interval_ = 0;
  Layout layout_;
  ColorInfo color_;
  std::int64_t default_duration_ = 1;
  std::int64_t last_pts_ = 0;
  std::uint64_t frames_sent_ = 0;
  Bytes extradata_;
  std::deque<Packet> packets_;
};

}  // namespace

int quantizer_for_quality(int quality) noexcept {
  // (quality, quantiser) anchor points; linear in between. 90 ("high") keeps
  // fine texture; 100 is lossless for VP9.
  static constexpr std::array<std::array<int, 2>, 7> anchors = {{
      {1, 63}, {20, 56}, {50, 44}, {75, 32}, {90, 20}, {99, 4}, {100, 0},
  }};
  quality = std::clamp(quality, 1, 100);
  for (std::size_t i = 1; i < anchors.size(); ++i) {
    const auto [q1, v1] = anchors[i];
    if (quality <= q1) {
      const auto [q0, v0] = anchors[i - 1];
      const double t = static_cast<double>(quality - q0) / static_cast<double>(q1 - q0);
      return static_cast<int>(std::lround(v0 + (v1 - v0) * t));
    }
  }
  return 0;
}

int cpu_used_for_preset(Codec codec, EncodePreset preset) noexcept {
  switch (preset) {
    case EncodePreset::Fast: return 4;
    case EncodePreset::Balanced: return codec == Codec::Vp9 ? 2 : 1;
    case EncodePreset::Small: return codec == Codec::Vp9 ? 1 : 0;
  }
  return 2;
}

std::uint32_t default_keyframe_interval(double fps) noexcept {
  // About ten seconds between keyframes: cheap seeking, little overhead.
  if (!(fps > 0) || !std::isfinite(fps)) return 300;
  return static_cast<std::uint32_t>(std::clamp(std::lround(fps * 10.0), 1l, 9999l));
}

std::unique_ptr<codec::VideoEncoder> make_encoder(Codec codec) { return std::make_unique<VpxEncoder>(codec); }

}  // namespace cvt::vpx
