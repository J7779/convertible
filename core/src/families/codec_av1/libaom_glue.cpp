// Convertible core - codec_av1 family: the only code that talks to the
// imported libaom.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/codec_av1/libaom_glue.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <string>

#include "aom/aom_decoder.h"
#include "aom/aom_encoder.h"
#include "aom/aomcx.h"
#include "aom/aomdx.h"

// One-time initialisers of libaom's static tables. Upstream guards them with
// aom_once(), which does not synchronise in a build without threads; they
// are therefore all run once here, under a C++ function-local static, before
// any codec instance exists (see core/src/imported/libaom/CONVERTIBLE_CHANGES.md).
extern "C" {
void av1_rtcd(void);
void aom_dsp_rtcd(void);
void aom_scale_rtcd(void);
void av1_init_intra_predictors(void);
void av1_init_me_luts(void);
void av1_rc_init_minq_luts(void);
}

namespace cvt::av1::detail {

namespace {

void ensure_initialised() {
  static const bool done = [] {
    av1_rtcd();
    aom_dsp_rtcd();
    aom_scale_rtcd();
    av1_init_intra_predictors();
    av1_init_me_luts();
    av1_rc_init_minq_luts();
    return true;
  }();
  (void)done;
}

std::string aom_message(aom_codec_ctx_t& ctx) {
  std::string message = aom_codec_error(&ctx) != nullptr ? aom_codec_error(&ctx) : "unknown error";
  const char* detail = aom_codec_error_detail(&ctx);
  if (detail != nullptr && *detail != '\0') {
    message += ": ";
    message += detail;
  }
  return message;
}

void put_sample(std::uint8_t* row, std::size_t x, std::uint32_t value, bool wide) {
  if (wide) {
    row[2 * x] = static_cast<std::uint8_t>(value & 0xFFu);
    row[2 * x + 1] = static_cast<std::uint8_t>(value >> 8);
  } else {
    row[x] = static_cast<std::uint8_t>(value);
  }
}

std::uint32_t get_sample(const std::uint8_t* row, std::size_t x, bool wide) {
  if (wide) return static_cast<std::uint32_t>(row[2 * x]) | (static_cast<std::uint32_t>(row[2 * x + 1]) << 8);
  return row[x];
}

Result<Picture> to_picture(const aom_image_t& img) {
  if (img.d_w == 0 || img.d_h == 0 || img.planes[0] == nullptr) {
    return fail(Status::Decode, "the AV1 decoder returned an empty picture");
  }
  const bool high = (img.fmt & AOM_IMG_FMT_HIGHBITDEPTH) != 0;
  const int coded_depth = img.bit_depth == 0 ? 8 : static_cast<int>(img.bit_depth);
  if (coded_depth != 8 && coded_depth != 10 && coded_depth != 12) {
    return fail(Status::Decode, "unsupported AV1 bit depth {}", coded_depth);
  }
  if (!high && coded_depth != 8) return fail(Status::Decode, "inconsistent AV1 picture format");
  Picture picture;
  picture.bit_depth = coded_depth;
  picture.monochrome = img.monochrome != 0;
  picture.color.color_primaries = static_cast<std::uint8_t>(img.cp);
  picture.color.transfer_characteristics = static_cast<std::uint8_t>(img.tc);
  picture.color.matrix_coefficients = static_cast<std::uint8_t>(img.mc);
  picture.color.full_range = img.range == AOM_CR_FULL_RANGE;
  picture.color.chroma_sample_position = static_cast<std::uint8_t>(img.csp);

  const bool out10 = coded_depth > 8;
  ir::PixelFormat format{};
  if (picture.monochrome || (img.x_chroma_shift == 1 && img.y_chroma_shift == 1)) {
    format = out10 ? ir::PixelFormat::I420P10 : ir::PixelFormat::I420;
  } else if (img.x_chroma_shift == 1 && img.y_chroma_shift == 0) {
    format = out10 ? ir::PixelFormat::I422P10 : ir::PixelFormat::I422;
  } else if (img.x_chroma_shift == 0 && img.y_chroma_shift == 0) {
    format = out10 ? ir::PixelFormat::I444P10 : ir::PixelFormat::I444;
  } else {
    return fail(Status::Decode, "unsupported AV1 chroma subsampling");
  }
  ir::VideoFrame frame = ir::VideoFrame::allocate(format, img.d_w, img.d_h);
  frame.color = color_info_from_cicp(picture.color, img.d_w, img.d_h);
  const int shift = coded_depth == 12 ? 2 : 0;
  const std::uint32_t max_value = out10 ? 1023u : 255u;
  for (std::size_t p = 0; p < 3; ++p) {
    const std::uint32_t cols = ir::plane_width(format, p, img.d_w);
    const std::uint32_t rows = ir::plane_height(format, p, img.d_h);
    ir::Plane& plane = frame.planes[p];
    if (p > 0 && (picture.monochrome || img.planes[p] == nullptr)) {
      const std::uint32_t neutral = out10 ? 512u : 128u;
      for (std::uint32_t y = 0; y < rows; ++y) {
        std::uint8_t* dst = plane.data.data() + plane.stride * y;
        for (std::uint32_t x = 0; x < cols; ++x) put_sample(dst, x, neutral, out10);
      }
      continue;
    }
    for (std::uint32_t y = 0; y < rows; ++y) {
      const unsigned char* src = img.planes[p] + static_cast<std::ptrdiff_t>(img.stride[p]) * y;
      std::uint8_t* dst = plane.data.data() + plane.stride * y;
      if (!high) {
        std::memcpy(dst, src, cols);
        continue;
      }
      for (std::uint32_t x = 0; x < cols; ++x) {
        std::uint16_t v = 0;
        std::memcpy(&v, src + 2 * x, 2);
        std::uint32_t value = shift != 0 ? (static_cast<std::uint32_t>(v) + (1u << (shift - 1))) >> shift : v;
        value = std::min(value, max_value);
        put_sample(dst, x, value, out10);
      }
    }
  }
  picture.frame = std::move(frame);
  return picture;
}

}  // namespace

ir::ColorInfo color_info_from_cicp(const PictureColor& color, std::uint32_t width, std::uint32_t height) {
  ir::ColorInfo info;
  switch (color.matrix_coefficients) {
    case 1: info.matrix = ir::ColorMatrix::BT709; break;
    case 5:
    case 6: info.matrix = ir::ColorMatrix::BT601; break;
    case 9:
    case 10: info.matrix = ir::ColorMatrix::BT2020; break;
    default: info.matrix = (width > 1024 || height > 576) ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601; break;
  }
  info.range = color.full_range ? ir::ColorRange::Full : ir::ColorRange::Limited;
  info.siting = color.chroma_sample_position == 2 ? ir::ChromaSiting::TopLeft : ir::ChromaSiting::Left;
  info.primaries = color.color_primaries;
  info.transfer = color.transfer_characteristics;
  return info;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct DecoderCore::Impl {
  aom_codec_ctx_t ctx{};
  bool open = false;
  std::deque<Picture> ready;

  ~Impl() {
    if (open) aom_codec_destroy(&ctx);
  }

  Result<void> collect() {
    aom_codec_iter_t iter = nullptr;
    while (const aom_image_t* img = aom_codec_get_frame(&ctx, &iter)) {
      CVT_TRY_ASSIGN(Picture picture, to_picture(*img));
      ready.push_back(std::move(picture));
    }
    return {};
  }
};

DecoderCore::DecoderCore() : impl_(std::make_unique<Impl>()) {}
DecoderCore::~DecoderCore() = default;

Result<void> DecoderCore::open(bool apply_film_grain) {
  ensure_initialised();
  aom_codec_dec_cfg_t cfg{};
  cfg.threads = 1;
  cfg.allow_lowbitdepth = 1;
  if (aom_codec_dec_init(&impl_->ctx, aom_codec_av1_dx(), &cfg, 0) != AOM_CODEC_OK) {
    return fail(Status::Internal, "cannot create the AV1 decoder");
  }
  impl_->open = true;
  if (aom_codec_control(&impl_->ctx, AV1D_SET_OUTPUT_ALL_LAYERS, 0) != AOM_CODEC_OK ||
      aom_codec_control(&impl_->ctx, AV1D_SET_OPERATING_POINT, 0) != AOM_CODEC_OK ||
      aom_codec_control(&impl_->ctx, AV1D_SET_SKIP_FILM_GRAIN, apply_film_grain ? 0 : 1) != AOM_CODEC_OK) {
    return fail(Status::Internal, "cannot configure the AV1 decoder");
  }
  return {};
}

Result<void> DecoderCore::decode(ByteView temporal_unit) {
  if (!impl_->open) return fail(Status::Internal, "AV1 decoder used before open()");
  if (temporal_unit.empty()) return {};
  const auto* data = reinterpret_cast<const std::uint8_t*>(temporal_unit.data());
  if (aom_codec_decode(&impl_->ctx, data, temporal_unit.size(), nullptr) != AOM_CODEC_OK) {
    return fail(Status::Decode, "AV1 decoding failed ({})", aom_message(impl_->ctx));
  }
  return impl_->collect();
}

Result<void> DecoderCore::flush() {
  if (!impl_->open) return {};
  if (aom_codec_decode(&impl_->ctx, nullptr, 0, nullptr) != AOM_CODEC_OK) {
    return fail(Status::Decode, "AV1 decoding failed ({})", aom_message(impl_->ctx));
  }
  return impl_->collect();
}

std::optional<Picture> DecoderCore::next_picture() {
  if (impl_->ready.empty()) return std::nullopt;
  Picture picture = std::move(impl_->ready.front());
  impl_->ready.pop_front();
  return picture;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct EncoderCore::Impl {
  aom_codec_ctx_t ctx{};
  aom_codec_enc_cfg_t cfg{};
  aom_image_t* image = nullptr;
  EncoderSettings settings;
  bool open = false;
  bool high = false;
  std::deque<EncodedUnit> ready;

  ~Impl() {
    if (image != nullptr) aom_img_free(image);
    if (open) aom_codec_destroy(&ctx);
  }

  /// Returns the number of packets collected.
  std::size_t collect() {
    std::size_t count = 0;
    aom_codec_iter_t iter = nullptr;
    while (const aom_codec_cx_pkt_t* pkt = aom_codec_get_cx_data(&ctx, &iter)) {
      if (pkt->kind != AOM_CODEC_CX_FRAME_PKT) continue;
      EncodedUnit unit;
      const auto* begin = static_cast<const std::byte*>(pkt->data.frame.buf);
      unit.data.assign(begin, begin + pkt->data.frame.sz);
      unit.pts_ms = pkt->data.frame.pts;
      unit.duration_ms = static_cast<std::int64_t>(pkt->data.frame.duration);
      unit.keyframe = (pkt->data.frame.flags & AOM_FRAME_IS_KEY) != 0;
      ready.push_back(std::move(unit));
      ++count;
    }
    return count;
  }
};

EncoderCore::EncoderCore() : impl_(std::make_unique<Impl>()) {}
EncoderCore::~EncoderCore() = default;

Result<void> EncoderCore::open(const EncoderSettings& s) {
  ensure_initialised();
  Impl& m = *impl_;
  m.settings = s;
  if (s.width == 0 || s.height == 0 || s.width > 65536 || s.height > 65536) {
    return fail(Status::InvalidArgument, "AV1 cannot encode a {}x{} picture", s.width, s.height);
  }
  aom_img_fmt_t fmt{};
  unsigned int profile = 0;
  switch (s.format) {
    case ir::PixelFormat::I420:
    case ir::PixelFormat::I420P10: fmt = AOM_IMG_FMT_I420; profile = 0; break;
    case ir::PixelFormat::I422:
    case ir::PixelFormat::I422P10: fmt = AOM_IMG_FMT_I422; profile = 2; break;
    case ir::PixelFormat::I444:
    case ir::PixelFormat::I444P10: fmt = AOM_IMG_FMT_I444; profile = 1; break;
    default: return fail(Status::InvalidArgument, "the AV1 encoder needs planar YCbCr input");
  }
  if (s.monochrome) {
    fmt = AOM_IMG_FMT_I420;
    profile = 0;
  }
  m.high = ir::bit_depth(s.format) > 8;
  const unsigned int usage = s.usage == Usage::Realtime   ? AOM_USAGE_REALTIME
                             : s.usage == Usage::AllIntra ? AOM_USAGE_ALL_INTRA
                                                          : AOM_USAGE_GOOD_QUALITY;
  aom_codec_iface_t* iface = aom_codec_av1_cx();
  if (aom_codec_enc_config_default(iface, &m.cfg, usage) != AOM_CODEC_OK) {
    return fail(Status::Internal, "cannot obtain the AV1 encoder defaults");
  }
  m.cfg.g_w = s.width;
  m.cfg.g_h = s.height;
  m.cfg.g_threads = 1;
  m.cfg.g_timebase.num = 1;
  m.cfg.g_timebase.den = 1000;
  m.cfg.g_profile = profile;
  m.cfg.g_bit_depth = m.high ? AOM_BITS_10 : AOM_BITS_8;
  m.cfg.g_input_bit_depth = m.high ? 10 : 8;
  m.cfg.monochrome = s.monochrome ? 1 : 0;
  m.cfg.g_lag_in_frames = s.usage == Usage::GoodQuality ? static_cast<unsigned int>(std::max(0, s.lag_in_frames)) : 0;
  if (s.still_picture) {
    m.cfg.g_limit = 1;
    m.cfg.kf_max_dist = 0;
  } else if (s.keyframe_interval > 0) {
    m.cfg.kf_min_dist = 0;
    m.cfg.kf_max_dist = s.keyframe_interval;
  }
  if (s.lossless) {
    m.cfg.rc_end_usage = AOM_Q;
    m.cfg.rc_min_quantizer = 0;
    m.cfg.rc_max_quantizer = 0;
  } else if (s.bitrate_kbps) {
    m.cfg.rc_end_usage = AOM_VBR;
    m.cfg.rc_target_bitrate = std::max<std::uint32_t>(1, *s.bitrate_kbps);
  } else {
    m.cfg.rc_end_usage = AOM_Q;
    m.cfg.rc_min_quantizer = 0;
    m.cfg.rc_max_quantizer = 63;
  }
  const aom_codec_flags_t flags = m.high ? AOM_CODEC_USE_HIGHBITDEPTH : 0;
  if (aom_codec_enc_init(&m.ctx, iface, &m.cfg, flags) != AOM_CODEC_OK) {
    return fail(Status::InvalidArgument, "cannot create the AV1 encoder ({})", aom_message(m.ctx));
  }
  m.open = true;
  bool ok = aom_codec_control(&m.ctx, AOME_SET_CPUUSED, s.cpu_used) == AOM_CODEC_OK;
  if (s.lossless) {
    ok = ok && aom_codec_control(&m.ctx, AV1E_SET_LOSSLESS, 1u) == AOM_CODEC_OK;
  } else if (!s.bitrate_kbps) {
    ok = ok && aom_codec_control(&m.ctx, AOME_SET_CQ_LEVEL, static_cast<unsigned int>(std::clamp(s.cq_level, 0, 63))) ==
                   AOM_CODEC_OK;
  }
  ok = ok &&
       aom_codec_control(&m.ctx, AV1E_SET_COLOR_PRIMARIES, static_cast<int>(s.color.color_primaries)) == AOM_CODEC_OK &&
       aom_codec_control(&m.ctx, AV1E_SET_TRANSFER_CHARACTERISTICS, static_cast<int>(s.color.transfer_characteristics)) ==
           AOM_CODEC_OK &&
       aom_codec_control(&m.ctx, AV1E_SET_MATRIX_COEFFICIENTS, static_cast<int>(s.color.matrix_coefficients)) ==
           AOM_CODEC_OK &&
       aom_codec_control(&m.ctx, AV1E_SET_COLOR_RANGE, s.color.full_range ? 1 : 0) == AOM_CODEC_OK &&
       aom_codec_control(&m.ctx, AV1E_SET_CHROMA_SAMPLE_POSITION, static_cast<int>(s.color.chroma_sample_position)) ==
           AOM_CODEC_OK;
  if (!ok) return fail(Status::InvalidArgument, "cannot configure the AV1 encoder ({})", aom_message(m.ctx));

  const aom_img_fmt_t img_fmt = m.high ? static_cast<aom_img_fmt_t>(fmt | AOM_IMG_FMT_HIGHBITDEPTH) : fmt;
  m.image = aom_img_alloc(nullptr, img_fmt, s.width, s.height, 32);
  if (m.image == nullptr) return fail(Status::Limit, "out of memory for a {}x{} AV1 picture", s.width, s.height);
  m.image->bit_depth = m.high ? 10 : 8;
  m.image->monochrome = s.monochrome ? 1 : 0;
  return {};
}

Bytes EncoderCore::sequence_header() const {
  Bytes out;
  if (!impl_->open) return out;
  aom_fixed_buf_t* headers = aom_codec_get_global_headers(&impl_->ctx);
  if (headers == nullptr) return out;
  if (headers->buf != nullptr) {
    const auto* begin = static_cast<const std::byte*>(headers->buf);
    out.assign(begin, begin + headers->sz);
    std::free(headers->buf);
  }
  std::free(headers);
  return out;
}

Result<void> EncoderCore::encode(const ir::VideoFrame& frame, std::int64_t pts_ms, std::int64_t duration_ms) {
  Impl& m = *impl_;
  if (!m.open) return fail(Status::Internal, "AV1 encoder used before open()");
  const ir::PixelFormat format = m.settings.format;
  if (frame.format != format || frame.width != m.settings.width || frame.height != m.settings.height || !frame.valid()) {
    return fail(Status::InvalidArgument, "frame does not match the AV1 encoder configuration");
  }
  aom_image_t& img = *m.image;
  const bool wide = m.high;
  const std::size_t planes = m.settings.monochrome ? 1 : 3;
  for (std::size_t p = 0; p < 3; ++p) {
    const std::uint32_t cols = p == 0 ? frame.width : ((frame.width + img.x_chroma_shift) >> img.x_chroma_shift);
    const std::uint32_t rows = p == 0 ? frame.height : ((frame.height + img.y_chroma_shift) >> img.y_chroma_shift);
    for (std::uint32_t y = 0; y < rows; ++y) {
      unsigned char* dst = img.planes[p] + static_cast<std::ptrdiff_t>(img.stride[p]) * y;
      if (p >= planes) {
        // Monochrome: libaom ignores chroma, but keep it defined.
        const std::uint16_t neutral = wide ? 512 : 128;
        for (std::uint32_t x = 0; x < cols; ++x) {
          if (wide) {
            std::memcpy(dst + 2 * x, &neutral, 2);
          } else {
            dst[x] = static_cast<unsigned char>(neutral);
          }
        }
        continue;
      }
      const std::uint8_t* src = frame.planes[p].data.data() + frame.planes[p].stride * y;
      if (!wide) {
        std::memcpy(dst, src, cols);
        continue;
      }
      for (std::uint32_t x = 0; x < cols; ++x) {
        const auto v = static_cast<std::uint16_t>(std::min<std::uint32_t>(get_sample(src, x, true), 1023u));
        std::memcpy(dst + 2 * x, &v, 2);
      }
    }
  }
  const auto duration = static_cast<unsigned long>(std::max<std::int64_t>(1, duration_ms));
  if (aom_codec_encode(&m.ctx, &img, pts_ms, duration, 0) != AOM_CODEC_OK) {
    return fail(Status::Encode, "AV1 encoding failed ({})", aom_message(m.ctx));
  }
  m.collect();
  return {};
}

Result<bool> EncoderCore::flush_step() {
  Impl& m = *impl_;
  if (!m.open) return false;
  if (aom_codec_encode(&m.ctx, nullptr, 0, 0, 0) != AOM_CODEC_OK) {
    return fail(Status::Encode, "AV1 encoding failed ({})", aom_message(m.ctx));
  }
  return m.collect() > 0;
}

std::optional<EncodedUnit> EncoderCore::next_unit() {
  if (impl_->ready.empty()) return std::nullopt;
  EncodedUnit unit = std::move(impl_->ready.front());
  impl_->ready.pop_front();
  return unit;
}

}  // namespace cvt::av1::detail
