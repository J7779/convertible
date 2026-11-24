// Convertible core - codec_hevc family: the VideoEncoder, a wrapper around
// Kvazaar (Tampere University, BSD-3-Clause) built single-threaded from its
// C sources (generic strategies only).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "cvt_kvazaar.h"

#include "codec/codec.hpp"
#include "codec/colorspace.hpp"
#include "families/codec_hevc/hevc_api.hpp"
#include "families/codec_hevc/hevc_internal.hpp"

namespace cvt::hevc {

bool ensure_encoder_ready() {
  // Kvazaar keeps its function tables in process-wide pointers. They are
  // filled once, before any encoder exists, and never change afterwards.
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] { ready = cvt_kvazaar_global_init() == 1; });
  return ready;
}

int quality_to_qp(int quality) noexcept {
  // (quality, QP) anchors, linear in between. 90 is visually transparent for
  // typical content; every step down saves bits.
  static constexpr std::array<std::array<int, 2>, 7> anchors = {
      {{1, 47}, {25, 39}, {50, 33}, {75, 28}, {90, 23}, {95, 19}, {100, 12}}};
  quality = std::clamp(quality, 1, 100);
  for (std::size_t i = 1; i < anchors.size(); ++i) {
    if (quality <= anchors[i][0]) {
      const int q0 = anchors[i - 1][0];
      const int q1 = anchors[i][0];
      const int p0 = anchors[i - 1][1];
      const int p1 = anchors[i][1];
      const double t = static_cast<double>(quality - q0) / static_cast<double>(q1 - q0);
      return static_cast<int>(std::lround(p0 + (p1 - p0) * t));
    }
  }
  return anchors.back()[1];
}

namespace {

struct KvazaarDeleter {
  void operator()(cvt_kvazaar_encoder* encoder) const noexcept { cvt_kvazaar_close(encoder); }
};
using KvazaarHandle = std::unique_ptr<cvt_kvazaar_encoder, KvazaarDeleter>;

struct MallocDeleter {
  void operator()(std::uint8_t* data) const noexcept { cvt_kvazaar_free(data); }
};
using MallocBuffer = std::unique_ptr<std::uint8_t, MallocDeleter>;

/// ITU-T H.273 code points for a colour description.
struct VuiColour {
  int primaries = 2;
  int transfer = 2;
  int matrix = 2;
};

[[nodiscard]] VuiColour vui_colour(ir::ColorMatrix matrix) noexcept {
  switch (matrix) {
    case ir::ColorMatrix::BT601: return {6, 6, 6};
    case ir::ColorMatrix::BT709: return {1, 1, 1};
    case ir::ColorMatrix::BT2020: return {9, 14, 9};
  }
  return {};
}

[[nodiscard]] int chroma_location(ir::ChromaSiting siting) noexcept {
  switch (siting) {
    case ir::ChromaSiting::Left: return 0;
    case ir::ChromaSiting::Center: return 1;
    case ir::ChromaSiting::TopLeft: return 2;
  }
  return 0;
}

[[nodiscard]] int kvazaar_preset(codec::EncodePreset preset) noexcept {
  switch (preset) {
    case codec::EncodePreset::Fast: return 0;      // ultrafast
    case codec::EncodePreset::Balanced: return 2;  // veryfast
    case codec::EncodePreset::Small: return 5;     // medium
  }
  return 2;
}

/// value * timescale / 1000 rounded to nearest.
[[nodiscard]] std::int64_t ms_to_ticks(std::int64_t ms, std::uint32_t timescale) noexcept {
  const std::int64_t scale = timescale;
  const std::int64_t whole = ms / 1000;
  const std::int64_t rest = ms % 1000;
  return whole * scale + (rest * scale + (rest >= 0 ? 500 : -500)) / 1000;
}

/// An I420 copy of `frame` padded to even dimensions by edge replication.
[[nodiscard]] codec::VideoFrame pad_to_even(const codec::VideoFrame& frame) {
  const std::uint32_t width = frame.width + (frame.width & 1u);
  const std::uint32_t height = frame.height + (frame.height & 1u);
  codec::VideoFrame out = codec::VideoFrame::allocate(ir::PixelFormat::I420, width, height);
  out.color = frame.color;
  out.pts_ms = frame.pts_ms;
  out.duration_ms = frame.duration_ms;
  for (std::size_t p = 0; p < 3; ++p) {
    const std::uint32_t src_w = ir::plane_width(frame.format, p, frame.width);
    const std::uint32_t src_h = ir::plane_height(frame.format, p, frame.height);
    const std::uint32_t dst_w = ir::plane_width(out.format, p, width);
    const std::uint32_t dst_h = ir::plane_height(out.format, p, height);
    for (std::uint32_t y = 0; y < dst_h; ++y) {
      const std::uint8_t* row = frame.planes[p].data.data() + std::size_t{std::min(y, src_h - 1)} * frame.planes[p].stride;
      std::uint8_t* dst = out.planes[p].data.data() + std::size_t{y} * out.planes[p].stride;
      std::memcpy(dst, row, src_w);
      for (std::uint32_t x = src_w; x < dst_w; ++x) dst[x] = row[src_w - 1];
    }
  }
  return out;
}

class HevcEncoder final : public codec::VideoEncoder {
 public:
  Result<void> configure(const codec::VideoEncodeParams& params) override {
    if (params.width == 0 || params.height == 0 || params.width > max_dimension || params.height > max_dimension ||
        std::uint64_t{params.width} * params.height > max_luma_samples) {
      return fail(Status::InvalidArgument, "HEVC cannot encode a {}x{} picture (at most level 6.2 sizes)",
                  params.width, params.height);
    }
    if (params.timescale == 0) return fail(Status::InvalidArgument, "the packet timescale must not be 0");
    if (!ensure_encoder_ready()) return fail(Status::Internal, "the HEVC encoder could not be initialised");
    params_ = params;
    width_ = params.width + (params.width & 1u);
    height_ = params.height + (params.height & 1u);

    cvt_kvazaar_params kp{};
    kp.width = static_cast<std::int32_t>(width_);
    kp.height = static_cast<std::int32_t>(height_);
    const bool fps_known = params.fps.num > 0 && params.fps.den > 0;
    // Kvazaar wants a fraction of 32-bit integers; reduce large ones.
    std::int64_t num = fps_known ? params.fps.num : 25;
    std::int64_t den = fps_known ? params.fps.den : 1;
    while (num > 0x7FFFFFFF || den > 0x7FFFFFFF) {
      num /= 2;
      den = std::max<std::int64_t>(1, den / 2);
    }
    kp.fps_num = static_cast<std::int32_t>(std::max<std::int64_t>(1, num));
    kp.fps_den = static_cast<std::int32_t>(den);
    if (fps_known) {
      nominal_duration_ = std::max<std::int64_t>(
          1, std::llround(static_cast<double>(params.timescale) * static_cast<double>(params.fps.den) /
                          static_cast<double>(params.fps.num)));
    }
    kp.qp = quality_to_qp(params.quality);
    if (params.bitrate_kbps && *params.bitrate_kbps > 0) {
      kp.bitrate = static_cast<std::int32_t>(std::min<std::uint64_t>(std::uint64_t{*params.bitrate_kbps} * 1000, 0x7FFFFFFF));
    }
    // Keyframes: the interval is a maximum. With hierarchical-B GOPs of 8,
    // Kvazaar places an IDR every `period + 1` frames and needs `period` to
    // be a multiple of 8; shorter intervals (and the fast preset) use I/P GOPs.
    const double fps = fps_known ? params.fps.value() : 25.0;
    std::uint32_t interval = params.keyframe_interval;
    if (interval == 0) {
      interval = static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::llround(fps * 5.0), 8, 600));
    }
    const std::uint32_t ra_period = interval > 8 ? (interval - 1) / 8 * 8 : 0;
    const bool random_access = params.preset != codec::EncodePreset::Fast && ra_period >= 8;
    kp.gop = random_access ? 8 : 0;
    kp.intra_period = static_cast<std::int32_t>(random_access ? ra_period : interval);
    kp.preset = kvazaar_preset(params.preset);
    const VuiColour vui = vui_colour(params.color.matrix);
    kp.colorprim = vui.primaries;
    kp.transfer = vui.transfer;
    kp.colormatrix = vui.matrix;
    kp.fullrange = params.color.range == ir::ColorRange::Full ? 1 : 0;
    kp.chroma_loc = chroma_location(params.color.siting);

    encoder_.reset(cvt_kvazaar_open(&kp));
    if (!encoder_) return fail(Status::InvalidArgument, "the HEVC encoder rejected the parameters");

    std::uint8_t* header_data = nullptr;
    std::size_t header_size = 0;
    if (cvt_kvazaar_headers(encoder_.get(), &header_data, &header_size) != 1) {
      return fail(Status::Encode, "the HEVC encoder could not write its parameter sets");
    }
    const MallocBuffer headers(header_data);
    const ByteView annexb(reinterpret_cast<const std::byte*>(header_data), header_size);
    const std::vector<ByteView> nals = split_annexb(annexb);
    std::vector<ByteView> parameter_sets;
    for (const ByteView nal : nals) {
      if (is_parameter_set(nal_type(nal))) parameter_sets.push_back(nal);
      if (nal_type(nal) == static_cast<std::uint8_t>(NalType::Sps)) {
        CVT_TRY_ASSIGN(const SequenceInfo sps, parse_sps(nal));
        reorder_ = sps.max_num_reorder_pics;
      }
    }
    CVT_TRY_ASSIGN(extradata_, build_hvcc(parameter_sets, 4));
    configured_ = true;
    return {};
  }

  Result<void> send_frame(const codec::VideoFrame& frame) override {
    if (!configured_) return fail(Status::InvalidArgument, "send_frame before configure");
    if (flushed_) return fail(Status::InvalidArgument, "send_frame after flush");
    if (!frame.valid()) return fail(Status::InvalidArgument, "invalid video frame");
    if (frame.width != params_.width || frame.height != params_.height) {
      return fail(Status::InvalidArgument, "frame of {}x{} sent to an encoder configured for {}x{}", frame.width,
                  frame.height, params_.width, params_.height);
    }
    const codec::VideoFrame* source = &frame;
    codec::VideoFrame converted;
    if (frame.format != ir::PixelFormat::I420) {
      CVT_TRY_ASSIGN(converted, codec::convert_frame(frame, ir::PixelFormat::I420, params_.color));
      source = &converted;
    }
    codec::VideoFrame padded;
    if (source->width != width_ || source->height != height_) {
      padded = pad_to_even(*source);
      source = &padded;
    }

    // Presentation timestamps must increase; repair any that do not.
    std::int64_t pts = ms_to_ticks(frame.pts_ms, params_.timescale);
    std::int64_t duration = frame.duration_ms > 0 ? ms_to_ticks(frame.duration_ms, params_.timescale) : nominal_duration_;
    if (!input_pts_.empty() && pts <= input_pts_.back()) {
      const std::int64_t step = input_durations_.back() > 0 ? input_durations_.back() : std::max<std::int64_t>(nominal_duration_, 1);
      pts = input_pts_.back() + step;
    }
    input_pts_.push_back(pts);
    input_durations_.push_back(duration);

    const std::array<const std::uint8_t*, 3> planes = {source->planes[0].data.data(), source->planes[1].data.data(),
                                                       source->planes[2].data.data()};
    const std::array<std::int32_t, 3> strides = {static_cast<std::int32_t>(source->planes[0].stride),
                                                 static_cast<std::int32_t>(source->planes[1].stride),
                                                 static_cast<std::int32_t>(source->planes[2].stride)};
    return encode(planes.data(), strides.data(), static_cast<std::int64_t>(input_pts_.size() - 1));
  }

  Result<std::optional<codec::Packet>> receive_packet() override {
    if (ready_.empty()) return std::optional<codec::Packet>{};
    codec::Packet packet = std::move(ready_.front());
    ready_.pop_front();
    return std::optional<codec::Packet>{std::move(packet)};
  }

  Bytes extradata() const override { return extradata_; }

  Result<void> flush() override {
    if (!configured_) return fail(Status::InvalidArgument, "flush before configure");
    if (flushed_) return {};
    flushed_ = true;
    // Every call releases at most one picture.
    for (std::size_t guard = 0; packets_out_ < input_pts_.size() && guard < input_pts_.size() + 64; ++guard) {
      CVT_TRY(encode(nullptr, nullptr, 0));
    }
    return {};
  }

 private:
  Result<void> encode(const std::uint8_t* const* planes, const std::int32_t* strides, std::int64_t index) {
    cvt_kvazaar_packet out{};
    if (cvt_kvazaar_encode(encoder_.get(), planes, strides, index, &out) != 1) {
      return fail(Status::Encode, "the HEVC encoder failed");
    }
    if (!out.data) return {};
    const MallocBuffer data(out.data);
    const ByteView annexb(reinterpret_cast<const std::byte*>(out.data), out.size);
    std::vector<ByteView> nals;
    for (const ByteView nal : split_annexb(annexb)) {
      const std::uint8_t type = nal_type(nal);
      // Parameter sets live in the configuration record; no access unit delimiters.
      if (is_parameter_set(type) || type == static_cast<std::uint8_t>(NalType::Aud)) continue;
      nals.push_back(nal);
    }
    codec::Packet packet;
    CVT_TRY_ASSIGN(packet.bytes, join_length_prefixed(nals, 4));
    if (out.pts < 0 || static_cast<std::size_t>(out.pts) >= input_pts_.size()) {
      return fail(Status::Internal, "the HEVC encoder returned an unknown picture");
    }
    const auto source = static_cast<std::size_t>(out.pts);
    packet.pts = input_pts_[source];
    packet.duration = input_durations_[source];
    packet.keyframe = is_irap(static_cast<std::uint8_t>(out.nal_unit_type));
    packet.dts = decode_timestamp(packet.pts);
    ++packets_out_;
    ready_.push_back(std::move(packet));
    return {};
  }

  /// DTS of the next packet: the presentation time of the frame with the same
  /// index in presentation order, shifted back by the reorder delay; kept
  /// strictly increasing and never after the packet's own PTS.
  [[nodiscard]] std::int64_t decode_timestamp(std::int64_t pts) {
    if (!delay_) {
      if (reorder_ == 0) {
        delay_ = 0;
      } else if (input_pts_.size() > reorder_) {
        delay_ = input_pts_[reorder_] - input_pts_[0];
      } else {
        const std::int64_t step = nominal_duration_ > 0 ? nominal_duration_ : std::max<std::int64_t>(1, input_durations_[0]);
        delay_ = step * static_cast<std::int64_t>(reorder_);
      }
    }
    std::int64_t dts = input_pts_[std::min(packets_out_, input_pts_.size() - 1)] - *delay_;
    if (last_dts_ && dts <= *last_dts_) dts = *last_dts_ + 1;
    dts = std::min(dts, pts);
    last_dts_ = dts;
    return dts;
  }

  codec::VideoEncodeParams params_;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::int64_t nominal_duration_ = 0;
  std::uint32_t reorder_ = 0;
  KvazaarHandle encoder_;
  Bytes extradata_;
  bool configured_ = false;
  bool flushed_ = false;
  std::vector<std::int64_t> input_pts_;
  std::vector<std::int64_t> input_durations_;
  std::size_t packets_out_ = 0;
  std::optional<std::int64_t> delay_;
  std::optional<std::int64_t> last_dts_;
  std::deque<codec::Packet> ready_;
};

}  // namespace

std::unique_ptr<codec::VideoEncoder> make_encoder() { return std::make_unique<HevcEncoder>(); }

}  // namespace cvt::hevc
