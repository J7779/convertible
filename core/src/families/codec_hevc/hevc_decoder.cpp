// Convertible core - codec_hevc family: the VideoDecoder, a wrapper around
// libhevc (Ittiam, Apache-2.0) built single-threaded from its C sources.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cvt_libhevc.h"

#include "codec/codec.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/codec_hevc/hevc_internal.hpp"

namespace cvt::hevc {

namespace {

struct LibhevcDeleter {
  void operator()(cvt_libhevc_decoder* decoder) const noexcept { cvt_libhevc_destroy(decoder); }
};
using LibhevcHandle = std::unique_ptr<cvt_libhevc_decoder, LibhevcDeleter>;

/// value * 1000 / timescale, rounded to nearest (ties away from zero).
[[nodiscard]] std::int64_t ticks_to_ms(std::int64_t ticks, std::uint32_t timescale) noexcept {
  if (timescale == 0) return 0;
  const std::int64_t scale = timescale;
  // Split to keep the product in range for any 64-bit tick value.
  const std::int64_t whole = ticks / scale;
  const std::int64_t rest = ticks % scale;
  const std::int64_t rest_ms = (rest * 1000 + (rest >= 0 ? scale / 2 : -(scale / 2))) / scale;
  return whole * 1000 + rest_ms;
}

/// True when every unit has a plausible NAL header (forbidden bit clear,
/// nuh_temporal_id_plus1 non-zero).
[[nodiscard]] bool plausible_nals(const std::vector<ByteView>& nals) noexcept {
  if (nals.empty()) return false;
  for (const ByteView nal : nals) {
    if (nal.size() < 2) return false;
    if ((std::to_integer<unsigned>(nal[0]) & 0x80u) != 0) return false;
    if ((std::to_integer<unsigned>(nal[1]) & 7u) == 0) return false;
  }
  return true;
}

[[nodiscard]] int output_layout(const SequenceInfo& sps) noexcept {
  switch (sps.chroma_format_idc) {
    case 2: return CVT_LIBHEVC_OUT_422;
    case 3: return CVT_LIBHEVC_OUT_444;
    default: return CVT_LIBHEVC_OUT_420;
  }
}

[[nodiscard]] Result<void> check_supported(const SequenceInfo& sps, std::uint64_t max_pixels) {
  if (sps.bit_depth_luma > 10 || sps.bit_depth_chroma > 10) {
    return fail(Status::Decode, "HEVC video with {}-bit samples is not supported (8 and 10 bit are)",
                std::max(sps.bit_depth_luma, sps.bit_depth_chroma));
  }
  if ((sps.bit_depth_luma > 8) != (sps.bit_depth_chroma > 8) && sps.chroma_format_idc != 0) {
    return fail(Status::Decode, "HEVC video with different luma and chroma bit depths is not supported");
  }
  if (sps.separate_colour_planes) {
    return fail(Status::Decode, "HEVC video coded as separate colour planes is not supported");
  }
  if (std::uint64_t{sps.width} * sps.height > max_luma_samples) {
    return fail(Status::Limit, "HEVC picture of {}x{} exceeds the largest supported size (level 6.2)", sps.width,
                sps.height);
  }
  if (max_pixels > 0 && std::uint64_t{sps.width} * sps.height > max_pixels) {
    return fail(Status::Limit, "HEVC picture of {}x{} exceeds the limit of {} pixels", sps.width, sps.height, max_pixels);
  }
  return {};
}

[[nodiscard]] std::string describe_error(std::uint32_t code) {
  return std::format("the HEVC stream is damaged or uses an unsupported feature (decoder error 0x{:X})", code & 0xFFu);
}

class HevcDecoder final : public codec::VideoDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    timescale_ = params.timescale == 0 ? 1000 : params.timescale;
    max_pixels_ = params.max_pixels;
    if (params.extradata.empty()) return {};
    const ByteView extra = params.extradata;
    std::vector<ByteView> nals;
    if (starts_with_start_code(extra)) {
      nals = split_annexb(extra);
      length_size_ = 0;
    } else {
      CVT_TRY_ASSIGN(config_, parse_hvcc(extra));
      length_size_ = config_.length_size;
      for (const Bytes& nal : config_.nals) nals.emplace_back(nal);
    }
    for (const ByteView nal : nals) CVT_TRY(remember_parameter_set(nal));
    return {};
  }

  Result<void> send_packet(const codec::Packet& packet) override {
    if (flushed_) return fail(Status::InvalidArgument, "send_packet after flush");
    CVT_TRY_ASSIGN(const std::vector<ByteView> nals, packet_nals(packet.bytes));
    for (const ByteView nal : nals) {
      if (is_parameter_set(nal_type(nal))) CVT_TRY(remember_parameter_set(nal));
    }
    if (!latest_sps_) return {};  // nothing can be decoded before the first SPS
    CVT_TRY(check_supported(*latest_sps_, max_pixels_));
    if (!decoder_ || needs_new_decoder(*latest_sps_)) CVT_TRY(start_decoder(*latest_sps_));

    // A new decoder first receives every parameter set known so far (from the
    // configuration record and earlier packets); copies in this packet are
    // then redundant.
    std::vector<ByteView> unit;
    const bool prepend = std::exchange(prepend_parameter_sets_, false);
    if (prepend) {
      for (const auto& entry : vps_by_id_) unit.emplace_back(entry.second);
      for (const auto& entry : sps_by_id_) unit.emplace_back(entry.second);
      for (const auto& entry : pps_by_id_) unit.emplace_back(entry.second);
    }
    for (const ByteView nal : nals) {
      if (!prepend || !is_parameter_set(nal_type(nal))) unit.push_back(nal);
    }
    const Bytes annexb = join_annexb(unit);

    if (unit.empty()) return {};
    const std::uint32_t timestamp = next_timestamp_++;
    timing_[timestamp] = {ticks_to_ms(packet.pts, timescale_),
                          packet.duration > 0 ? ticks_to_ms(packet.duration, timescale_) : 0};
    // Pictures the decoder drops (undecodable leading pictures, damaged data)
    // never claim their timing; forget the oldest entries.
    while (timing_.size() > 64) timing_.erase(timing_.begin());
    return decode(annexb, timestamp);
  }

  Result<std::optional<codec::VideoFrame>> receive_frame() override {
    if (ready_.empty()) return std::optional<codec::VideoFrame>{};
    codec::VideoFrame& front = ready_.front();
    if (front.duration_ms <= 0) {
      if (ready_.size() >= 2) {
        const std::int64_t gap = ready_[1].pts_ms - front.pts_ms;
        front.duration_ms = gap > 0 ? gap : last_duration_ms_;
      } else if (!flushed_) {
        return std::optional<codec::VideoFrame>{};  // wait for the next picture to learn the duration
      } else {
        front.duration_ms = last_duration_ms_;
      }
    }
    last_duration_ms_ = front.duration_ms;
    codec::VideoFrame frame = std::move(front);
    ready_.pop_front();
    return std::optional<codec::VideoFrame>{std::move(frame)};
  }

  Result<void> flush() override {
    if (flushed_) return {};
    flushed_ = true;
    return drain();
  }

 private:
  struct Timing {
    std::int64_t pts_ms = 0;
    std::int64_t duration_ms = 0;
  };

  [[nodiscard]] Result<std::vector<ByteView>> packet_nals(ByteView data) const {
    if (length_size_ != 0) {
      if (auto nals = split_length_prefixed(data, length_size_); nals && plausible_nals(*nals)) return *nals;
    }
    if (starts_with_start_code(data)) return split_annexb(data);
    if (length_size_ == 0) {
      // No configuration record: accept 4-byte length prefixes too.
      if (auto nals = split_length_prefixed(data, 4); nals && plausible_nals(*nals)) return *nals;
    }
    if (data.empty()) return std::vector<ByteView>{};
    return fail(Status::Decode, "an HEVC packet holds neither length-prefixed nor Annex-B NAL units");
  }

  Result<void> remember_parameter_set(ByteView nal) {
    const std::uint8_t type = nal_type(nal);
    if (nal.size() < 3) return {};
    const std::uint32_t id_bits = std::to_integer<std::uint32_t>(nal[2]);
    if (type == 32) {
      vps_by_id_[(id_bits >> 4) & 0xF] = Bytes(nal.begin(), nal.end());
    } else if (type == 33) {
      auto parsed = parse_sps(nal);
      if (!parsed) return {};  // a damaged SPS is left to the decoder, which ignores it
      sps_by_id_[parsed->sps_id] = Bytes(nal.begin(), nal.end());
      latest_sps_ = *parsed;
    } else if (type == 34) {
      auto parsed = parse_pps_lite(nal);
      if (!parsed) return {};
      pps_by_id_[parsed->pps_id] = Bytes(nal.begin(), nal.end());
    }
    return {};
  }

  [[nodiscard]] bool needs_new_decoder(const SequenceInfo& sps) const noexcept {
    return sps.width != active_.width || sps.height != active_.height ||
           sps.chroma_format_idc != active_.chroma_format_idc || sps.bit_depth_luma != active_.bit_depth_luma ||
           sps.bit_depth_chroma != active_.bit_depth_chroma;
  }

  Result<void> start_decoder(const SequenceInfo& sps) {
    if (decoder_) {
      // A new sequence format: release what the old decoder still holds.
      CVT_TRY(drain());
      decoder_.reset();
    }
    decoder_.reset(cvt_libhevc_create(output_layout(sps)));
    if (!decoder_) return fail(Status::Decode, "the HEVC decoder could not be created (out of memory)");
    active_ = sps;
    format_ = pixel_format(sps);
    color_ = color_info(sps);
    prepend_parameter_sets_ = true;
    draining_ = false;
    return {};
  }

  [[nodiscard]] codec::VideoFrame new_buffer() const {
    codec::VideoFrame frame = codec::VideoFrame::allocate(format_, active_.width, active_.height);
    frame.color = color_;
    if (active_.sar_width > 0 && active_.sar_height > 0) frame.sample_aspect = {active_.sar_width, active_.sar_height};
    return frame;
  }

  void deliver(codec::VideoFrame&& frame, std::uint32_t timestamp) {
    if constexpr (std::endian::native == std::endian::big) {
      if (ir::bytes_per_sample(frame.format) == 2) {
        for (auto& plane : frame.planes) {
          for (std::size_t i = 0; i + 1 < plane.data.size(); i += 2) std::swap(plane.data[i], plane.data[i + 1]);
        }
      }
    }
    const auto timing = timing_.find(timestamp);
    if (timing != timing_.end()) {
      frame.pts_ms = timing->second.pts_ms;
      frame.duration_ms = timing->second.duration_ms;
      timing_.erase(timing);
    }
    ready_.push_back(std::move(frame));
  }

  /// One decode call with output buffer `frame`; delivers the output if any.
  Result<cvt_libhevc_io> call(const std::uint8_t* data, std::uint32_t size, std::uint32_t timestamp) {
    codec::VideoFrame frame = new_buffer();
    cvt_libhevc_io io{};
    for (std::size_t p = 0; p < 3; ++p) {
      io.planes[p] = frame.planes[p].data.data();
      io.plane_sizes[p] = static_cast<std::uint32_t>(frame.planes[p].data.size());
    }
    const int rc = cvt_libhevc_decode(decoder_.get(), data, size, timestamp, &io);
    if (io.output_present) {
      if (io.width != active_.width || io.height != active_.height) {
        return fail(Status::Decode, "the HEVC decoder produced a {}x{} picture for a {}x{} stream", io.width,
                    io.height, active_.width, active_.height);
      }
      deliver(std::move(frame), io.output_timestamp);
    }
    if (rc != 0 && io.fatal && !draining_) return fail(Status::Decode, "{}", describe_error(io.error_code));
    return io;
  }

  Result<void> decode(const Bytes& annexb, std::uint32_t timestamp) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(annexb.data());
    if (annexb.size() > 0x7FFFFFFFu) return fail(Status::Limit, "an HEVC access unit is too large");
    std::uint32_t offset = 0;
    const auto size = static_cast<std::uint32_t>(annexb.size());
    // Each call consumes at least one NAL unit, so this bound is generous.
    for (std::uint32_t guard = 0; offset < size && guard < 4096; ++guard) {
      CVT_TRY_ASSIGN(const cvt_libhevc_io io, call(data + offset, size - offset, timestamp));
      if (io.bytes_consumed == 0 || io.bytes_consumed > size - offset) break;
      offset += io.bytes_consumed;
      if (size - offset <= 3) break;  // only a partial start code is left
    }
    return {};
  }

  Result<void> drain() {
    if (!decoder_) return {};
    if (cvt_libhevc_flush(decoder_.get()) != 0) return {};
    draining_ = true;
    for (int guard = 0; guard < 64; ++guard) {
      CVT_TRY_ASSIGN(const cvt_libhevc_io io, call(nullptr, 0, 0));
      if (!io.output_present) break;
    }
    draining_ = false;
    return {};
  }

  std::uint32_t timescale_ = 1000;
  std::uint64_t max_pixels_ = 0;  ///< caller limit (CodecParams::max_pixels); 0 = none
  unsigned length_size_ = 0;
  DecoderConfig config_;
  std::map<std::uint32_t, Bytes> vps_by_id_;
  std::map<std::uint32_t, Bytes> sps_by_id_;
  std::map<std::uint32_t, Bytes> pps_by_id_;
  std::optional<SequenceInfo> latest_sps_;
  LibhevcHandle decoder_;
  SequenceInfo active_;
  ir::PixelFormat format_ = ir::PixelFormat::I420;
  ir::ColorInfo color_;
  bool prepend_parameter_sets_ = false;
  bool draining_ = false;
  bool flushed_ = false;
  std::uint32_t next_timestamp_ = 0;
  std::map<std::uint32_t, Timing> timing_;
  std::deque<codec::VideoFrame> ready_;
  std::int64_t last_duration_ms_ = 0;
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_decoder() { return std::make_unique<HevcDecoder>(); }

}  // namespace cvt::hevc
