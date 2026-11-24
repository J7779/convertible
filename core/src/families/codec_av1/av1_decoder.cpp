// Convertible core - codec_av1 family: the AV1 stream decoder
// (codec::VideoDecoder for codec::id::av1).
//
// Input packets may be Section 5 ("low overhead": MP4, Matroska, IVF, .obu)
// or Annex B temporal units; the layout is detected on the first packet and
// Annex B is rewritten to Section 5. A packet may hold several temporal units
// (split at temporal delimiters). Every sequence header is parsed in-house and
// checked against the size limits before libaom sees it. When the stream
// starts without a sequence header, the one from the av1C extradata is
// inserted. AV1 never reorders temporal units, so each shown picture
// (including show_existing_frame) takes the timestamp of the temporal unit it
// came from; when packets carry no duration, a picture's duration is the
// distance to the next one, so one picture is held back until then.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "families/codec_av1/av1_codec.hpp"
#include "families/codec_av1/obu.hpp"

namespace cvt::av1 {

namespace {

std::int64_t rescale(std::int64_t value, std::uint32_t from, std::uint32_t to) {
  if (from == 0) return 0;
  if (value < 0) {
    if (value == std::numeric_limits<std::int64_t>::min()) ++value;
    return -rescale(-value, from, to);
  }
  const std::int64_t q = value / from;
  const std::int64_t r = value % from;
  if (to != 0 && q > std::numeric_limits<std::int64_t>::max() / to - 1) return std::numeric_limits<std::int64_t>::max();
  return q * to + (r * to + from / 2) / from;
}

/// An identity-matrix picture (planes G, B, R; 4:4:4 unless the stream is
/// malformed) as RGBA8, expanded from limited range when the stream says so.
codec::VideoFrame gbr_to_rgba(const Picture& picture) {
  const ir::VideoFrame& in = picture.frame;
  codec::VideoFrame out = codec::VideoFrame::allocate(ir::PixelFormat::RGBA8, in.width, in.height);
  out.pts_ms = in.pts_ms;
  out.duration_ms = in.duration_ms;
  const bool wide = ir::bytes_per_sample(in.format) == 2;
  const int sx = ir::chroma_shift_x(in.format);
  const int sy = ir::chroma_shift_y(in.format);
  const bool full = picture.color.full_range;
  const auto sample = [&](std::size_t plane, std::uint32_t x, std::uint32_t y) -> std::uint32_t {
    if (plane > 0) {
      x >>= sx;
      y >>= sy;
    }
    const ir::Plane& p = in.planes[plane];
    const std::uint8_t* row = p.data.data() + p.stride * y;
    std::uint32_t v = wide ? (static_cast<std::uint32_t>(row[2 * x]) | (static_cast<std::uint32_t>(row[2 * x + 1]) << 8)) : row[x];
    if (wide) v = (v + 2) >> 2;  // 10 -> 8 bits
    if (!full) v = static_cast<std::uint32_t>(std::clamp((static_cast<int>(v) - 16) * 255 / 219, 0, 255));
    return std::min<std::uint32_t>(v, 255);
  };
  for (std::uint32_t y = 0; y < in.height; ++y) {
    std::uint8_t* dst = out.planes[0].data.data() + out.planes[0].stride * y;
    for (std::uint32_t x = 0; x < in.width; ++x) {
      dst[4 * x] = static_cast<std::uint8_t>(sample(2, x, y));      // R
      dst[4 * x + 1] = static_cast<std::uint8_t>(sample(0, x, y));  // G
      dst[4 * x + 2] = static_cast<std::uint8_t>(sample(1, x, y));  // B
      dst[4 * x + 3] = 255;
    }
  }
  return out;
}

class Av1Decoder final : public codec::VideoDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    if (configured_) return fail(Status::Internal, "AV1 decoder configured twice");
    configured_ = true;
    timescale_ = params.timescale == 0 ? 1000 : params.timescale;
    if (params.max_pixels > 0) max_pixels_ = std::min(max_pixels_, params.max_pixels);
    const ByteView extra = params.extradata;
    if (!extra.empty()) {
      if ((std::to_integer<std::uint8_t>(extra[0]) & 0x80u) != 0) {
        CVT_TRY_ASSIGN(const Av1Config config, parse_av1c(extra));
        config_obus_ = config.config_obus;
      } else if (is_section5(extra, true)) {
        config_obus_.assign(extra.begin(), extra.end());
      } else {
        return fail(Status::Decode, "unrecognised AV1 codec configuration");
      }
      // Keep only sequence headers and metadata (no frames, no delimiters).
      CVT_TRY_ASSIGN(const std::vector<Obu> obus, split_section5(config_obus_));
      Bytes kept;
      for (const Obu& obu : obus) {
        if (obu.type != ObuType::SequenceHeader && obu.type != ObuType::Metadata) continue;
        if (obu.type == ObuType::SequenceHeader) CVT_TRY(check_sequence_header(obu.payload));
        append_obu(kept, obu);
      }
      config_obus_ = std::move(kept);
    }
    return core_.open(true);
  }

  Result<void> send_packet(const codec::Packet& packet) override {
    if (!configured_) return fail(Status::Internal, "AV1 decoder used before configure()");
    if (flushed_) return fail(Status::Internal, "AV1 decoder fed after flush()");
    if (packet.bytes.empty()) return {};
    ByteView data = packet.bytes;
    if (layout_ == Layout::Unknown) {
      if (is_section5(data)) {
        layout_ = Layout::Section5;
      } else if (is_annexb(data)) {
        layout_ = Layout::AnnexB;
      } else if (is_section5(data, true)) {
        layout_ = Layout::Section5;
      } else {
        return fail(Status::Decode, "the packet is not an AV1 bitstream");
      }
    }
    Bytes converted;
    if (layout_ == Layout::AnnexB) {
      CVT_TRY_ASSIGN(converted, annexb_to_section5(data));
      data = converted;
    }
    CVT_TRY_ASSIGN(const std::vector<ByteView> units, split_temporal_units(data));
    for (const ByteView unit : units) {
      CVT_TRY(decode_unit(unit, packet.pts, packet.duration));
    }
    return {};
  }

  Result<std::optional<codec::VideoFrame>> receive_frame() override {
    if (ready_.empty()) return std::optional<codec::VideoFrame>();
    codec::VideoFrame frame = std::move(ready_.front());
    ready_.pop_front();
    return std::optional<codec::VideoFrame>(std::move(frame));
  }

  Result<void> flush() override {
    if (!configured_) return fail(Status::Internal, "AV1 decoder used before configure()");
    if (flushed_) return {};
    flushed_ = true;
    CVT_TRY(core_.flush());
    collect(last_pts_, 0);
    if (pending_) {
      pending_->duration_ms = last_duration_ms_;
      ready_.push_back(std::move(*pending_));
      pending_.reset();
    }
    return {};
  }

 private:
  enum class Layout { Unknown, Section5, AnnexB };

  Result<void> check_sequence_header(ByteView payload) {
    CVT_TRY_ASSIGN(const SequenceHeader header, parse_sequence_header(payload));
    if (header.max_frame_width > max_video_side || header.max_frame_height > max_video_side ||
        std::uint64_t{header.max_frame_width} * header.max_frame_height > max_pixels_) {
      return fail(Status::Limit, "AV1 frames of up to {}x{} exceed the supported size", header.max_frame_width,
                  header.max_frame_height);
    }
    return {};
  }

  Result<void> decode_unit(ByteView unit, std::int64_t pts, std::int64_t duration) {
    CVT_TRY_ASSIGN(const std::vector<Obu> obus, split_section5(unit));
    bool has_sequence_header = false;
    for (const Obu& obu : obus) {
      if (obu.type != ObuType::SequenceHeader) continue;
      CVT_TRY(check_sequence_header(obu.payload));
      has_sequence_header = true;
    }
    if (has_sequence_header) seen_sequence_header_ = true;
    if (!seen_sequence_header_ && !config_obus_.empty()) {
      // Insert the configuration OBUs after a leading temporal delimiter.
      Bytes joined;
      joined.reserve(unit.size() + config_obus_.size());
      std::size_t first = 0;
      if (!obus.empty() && obus[0].type == ObuType::TemporalDelimiter) {
        append_obu(joined, obus[0]);
        first = 1;
      }
      joined.insert(joined.end(), config_obus_.begin(), config_obus_.end());
      for (std::size_t i = first; i < obus.size(); ++i) append_obu(joined, obus[i]);
      seen_sequence_header_ = true;
      CVT_TRY(core_.decode(joined));
    } else {
      CVT_TRY(core_.decode(unit));
    }
    collect(pts, duration);
    return {};
  }

  void collect(std::int64_t pts, std::int64_t duration) {
    while (std::optional<Picture> picture = core_.next_picture()) {
      // Identity-matrix (GBR) pictures cannot be described by ir::ColorInfo:
      // deliver them as RGBA so that every consumer sees the right colours.
      codec::VideoFrame frame = picture->color.matrix_coefficients == 0 ? gbr_to_rgba(*picture) : std::move(picture->frame);
      frame.pts_ms = rescale(pts, timescale_, 1000);
      frame.duration_ms = duration > 0 ? rescale(duration, timescale_, 1000) : 0;
      last_pts_ = pts;
      if (pending_) {
        const std::int64_t delta = frame.pts_ms - pending_->pts_ms;
        if (delta > 0) last_duration_ms_ = delta;
        pending_->duration_ms = last_duration_ms_;
        ready_.push_back(std::move(*pending_));
        pending_.reset();
      }
      if (frame.duration_ms > 0) {
        last_duration_ms_ = frame.duration_ms;
        ready_.push_back(std::move(frame));
      } else {
        pending_ = std::move(frame);
      }
    }
  }

  detail::DecoderCore core_;
  Bytes config_obus_;
  std::uint32_t timescale_ = 1000;
  std::uint64_t max_pixels_ = max_video_pixels;  ///< lowered by CodecParams::max_pixels
  Layout layout_ = Layout::Unknown;
  bool configured_ = false;
  bool flushed_ = false;
  bool seen_sequence_header_ = false;
  std::deque<codec::VideoFrame> ready_;
  std::optional<codec::VideoFrame> pending_;
  std::int64_t last_pts_ = 0;
  std::int64_t last_duration_ms_ = 0;
};

}  // namespace

std::int64_t ticks_to_ms(std::int64_t ticks, std::uint32_t timescale) { return rescale(ticks, timescale, 1000); }
std::int64_t ms_to_ticks(std::int64_t ms, std::uint32_t timescale) { return rescale(ms, 1000, timescale); }

std::unique_ptr<codec::VideoDecoder> make_decoder() { return std::make_unique<Av1Decoder>(); }

}  // namespace cvt::av1
