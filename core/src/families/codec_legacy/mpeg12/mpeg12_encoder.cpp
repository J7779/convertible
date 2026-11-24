// Convertible core - codec_legacy family: MPEG-1 / MPEG-2 (Main Profile) video encoder.
//
// Produces I, P and B pictures (progressive frame pictures, 4:2:0), one slice
// per macroblock row, default quantiser matrices, with motion estimation at
// half-sample precision. Constant quality from `quality` (quantiser scale per
// picture type) or a simple per-picture rate control from `bitrate_kbps`.
// Every I picture is preceded by a sequence header and a GOP header, so the
// stream can be cut and played from any I picture; the out-of-band
// extradata repeats the sequence header (and the MPEG-2 sequence extension).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/dct.hpp"
#include "families/codec_legacy/mpeg12/mpeg12.hpp"
#include "families/codec_legacy/mpeg12/mpeg12_common.hpp"
#include "families/codec_legacy/mpeg12/mpeg12_tables.hpp"

namespace cvt::legacy::mpeg12 {

namespace {

constexpr int kI = 1;
constexpr int kP = 2;
constexpr int kB = 3;

/// Encoder-side lookup of a prefix code by symbol.
struct Code {
  std::uint32_t bits = 0;
  int length = 0;
};

template <std::size_t N>
[[nodiscard]] Code find_code(const VlcCode (&table)[N], int symbol) {
  for (const VlcCode& c : table) {
    if (c.symbol == symbol) return {c.code, c.length};
  }
  return {};
}

/// Run/level -> code for the two DCT tables (without the sign bit).
struct DctCodes {
  std::array<std::array<Code, 41>, 32> rl{};
  Code eob;
  template <std::size_t N>
  explicit DctCodes(const VlcCode (&table)[N]) {
    for (const VlcCode& c : table) {
      if (c.symbol == kDctEob) {
        eob = {c.code, c.length};
      } else if (c.symbol != kDctEscape) {
        const int run = c.symbol >> 6;
        const int level = c.symbol & 63;
        rl[static_cast<std::size_t>(run)][static_cast<std::size_t>(level)] = {c.code, c.length};
      }
    }
  }
};

/// An MB-aligned 4:2:0 picture.
struct Planes {
  std::array<std::vector<std::uint8_t>, 3> p;
  std::array<int, 3> stride{};
  std::array<int, 3> rows{};
  void allocate(int w, int h) {
    stride = {w, w / 2, w / 2};
    rows = {h, h / 2, h / 2};
    for (int i = 0; i < 3; ++i) p[static_cast<std::size_t>(i)].assign(static_cast<std::size_t>(stride[static_cast<std::size_t>(i)]) * static_cast<std::size_t>(rows[static_cast<std::size_t>(i)]), 128);
  }
  [[nodiscard]] RefPlane ref(int i) const {
    return {p[static_cast<std::size_t>(i)].data(), stride[static_cast<std::size_t>(i)], stride[static_cast<std::size_t>(i)], rows[static_cast<std::size_t>(i)]};
  }
  [[nodiscard]] std::uint8_t* at(int i, int x, int y) {
    return p[static_cast<std::size_t>(i)].data() + static_cast<std::ptrdiff_t>(y) * stride[static_cast<std::size_t>(i)] + x;
  }
  [[nodiscard]] const std::uint8_t* at(int i, int x, int y) const {
    return p[static_cast<std::size_t>(i)].data() + static_cast<std::ptrdiff_t>(y) * stride[static_cast<std::size_t>(i)] + x;
  }
};

struct InputFrame {
  Planes planes;
  std::int64_t pts_ms = 0;
  std::int64_t duration_ms = 0;
  std::uint64_t display_index = 0;
};

struct Vec {
  int x = 0;  // half-sample units
  int y = 0;
  friend bool operator==(const Vec&, const Vec&) = default;
};

[[nodiscard]] int sad_block(const std::uint8_t* a, int as, const std::uint8_t* b, int bs, int w, int h) {
  int sum = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) sum += std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x]));
    a += as;
    b += bs;
  }
  return sum;
}

class Mpeg12Encoder final : public codec::VideoEncoder {
 public:
  explicit Mpeg12Encoder(bool mpeg2) : mpeg2_(mpeg2) {}

  Result<void> configure(const VideoEncodeParams& params) override {
    if (params.width == 0 || params.height == 0) return fail(Status::InvalidArgument, "the picture has no pixels");
    if (!mpeg2_ && (params.width > 4095 || params.height > 4095)) {
      return fail(Status::InvalidArgument, "MPEG-1 video is limited to 4095x4095 pixels");
    }
    if (params.width > 16383 || params.height > 2800) {
      return fail(Status::InvalidArgument, "picture size {}x{} is too large for MPEG-2 video", params.width,
                  params.height);
    }
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 90000;
    width_ = static_cast<int>(params.width);
    height_ = static_cast<int>(params.height);
    mb_width_ = (width_ + 15) / 16;
    mb_height_ = (height_ + 15) / 16;
    choose_frame_rate();
    gop_ = params.keyframe_interval > 0 ? static_cast<int>(std::min<std::uint32_t>(params.keyframe_interval, 300)) : 12;
    b_frames_ = params.preset == EncodePreset::Fast ? 0 : 2;
    if (gop_ <= b_frames_) b_frames_ = std::max(0, gop_ - 1);
    search_range_ = params.preset == EncodePreset::Small ? 24 : (params.preset == EncodePreset::Fast ? 8 : 16);
    base_q_ = std::clamp(static_cast<int>(std::lround(quality_to_scale(params.quality, 1.0, 2.0, 31.0))), 1, 31);
    if (params.bitrate_kbps) {
      bitrate_ = static_cast<double>(*params.bitrate_kbps) * 1000.0;
      rc_q_ = 6.0;
    }
    color_ = params.color;
    color_.range = ir::ColorRange::Limited;
    if (!mpeg2_) color_.matrix = ir::ColorMatrix::BT601;
    color_.siting = mpeg2_ ? ir::ChromaSiting::Left : ir::ChromaSiting::Center;
    for (Planes* p : {&ref_fwd_, &ref_bwd_, &recon_}) p->allocate(mb_width_ * 16, mb_height_ * 16);
    mvs_.assign(static_cast<std::size_t>(mb_width_ * mb_height_), Vec{});
    sequence_header_ = build_sequence_header();
    return {};
  }

  Result<void> send_frame(const VideoFrame& frame) override {
    if (frame.width != params_.width || frame.height != params_.height) {
      return fail(Status::InvalidArgument, "frame size {}x{} differs from the configured {}x{}", frame.width,
                  frame.height, params_.width, params_.height);
    }
    CVT_TRY_ASSIGN(const VideoFrame yuv, to_format(frame, PixelFormat::I420, color_));
    InputFrame in;
    in.planes.allocate(mb_width_ * 16, mb_height_ * 16);
    for (int p = 0; p < 3; ++p) {
      const int w = static_cast<int>(ir::plane_width(PixelFormat::I420, static_cast<std::size_t>(p), params_.width));
      const int h = static_cast<int>(ir::plane_height(PixelFormat::I420, static_cast<std::size_t>(p), params_.height));
      const auto& src = yuv.planes[static_cast<std::size_t>(p)];
      for (int y = 0; y < in.planes.rows[static_cast<std::size_t>(p)]; ++y) {
        const std::uint8_t* s = src.data.data() + static_cast<std::size_t>(std::min(y, h - 1)) * src.stride;
        std::uint8_t* d = in.planes.at(p, 0, y);
        std::memcpy(d, s, static_cast<std::size_t>(w));
        for (int x = w; x < in.planes.stride[static_cast<std::size_t>(p)]; ++x) d[x] = s[w - 1];  // edge replication
      }
    }
    in.pts_ms = frame.pts_ms;
    in.duration_ms = frame.duration_ms;
    in.display_index = input_count_++;
    display_pts_.push_back(frame.pts_ms);

    const int position = static_cast<int>(in.display_index % static_cast<std::uint64_t>(gop_));
    const bool anchor = position == 0 || b_frames_ == 0 || (position % (b_frames_ + 1)) == 0;
    if (!anchor) {
      pending_b_.push_back(std::move(in));
      return {};
    }
    return encode_anchor_and_bs(std::move(in), position == 0 ? kI : kP);
  }

  Result<std::optional<Packet>> receive_packet() override {
    if (ready_.empty()) return std::optional<Packet>{};
    Packet packet = std::move(ready_.front());
    ready_.pop_front();
    return std::optional<Packet>(std::move(packet));
  }

  Bytes extradata() const override { return to_cvt_bytes(sequence_header_); }

  Result<void> flush() override {
    if (!pending_b_.empty()) {
      // The last pictures have no future reference: the newest becomes a P picture.
      InputFrame last = std::move(pending_b_.back());
      pending_b_.pop_back();
      CVT_TRY(encode_anchor_and_bs(std::move(last), kP));
    }
    // A sequence end code after the last picture.
    if (!ready_.empty()) {
      Bytes& tail = ready_.back().bytes;
      for (const std::uint8_t b : {0x00, 0x00, 0x01, 0xB7}) tail.push_back(static_cast<std::byte>(b));
    }
    return {};
  }

 private:
  // ---- rate and timing ----------------------------------------------------------

  void choose_frame_rate() {
    const double fps = params_.fps.value() > 0 ? params_.fps.value() : 25.0;
    double best = 1e9;
    for (int code = 1; code <= 8; ++code) {
      const double r = static_cast<double>(kFrameRates[code].num) / static_cast<double>(kFrameRates[code].den);
      const int max_n = mpeg2_ ? 3 : 0;
      const int max_d = mpeg2_ ? 31 : 0;
      for (int n = 0; n <= max_n; ++n) {
        for (int d = 0; d <= max_d; ++d) {
          const double v = r * (n + 1) / (d + 1);
          const double err = std::abs(v - fps) / fps + (n + d) * 1e-9;
          if (err < best) {
            best = err;
            frame_rate_code_ = code;
            frame_rate_n_ = n;
            frame_rate_d_ = d;
          }
        }
      }
    }
    fps_ = static_cast<double>(kFrameRates[frame_rate_code_].num) / static_cast<double>(kFrameRates[frame_rate_code_].den) *
           (frame_rate_n_ + 1) / (frame_rate_d_ + 1);
  }

  [[nodiscard]] int picture_q(int type) const {
    double q = bitrate_ > 0 ? rc_q_ : static_cast<double>(base_q_);
    if (type == kB) q *= 1.25;
    if (type == kI && bitrate_ > 0) q *= 0.85;
    return std::clamp(static_cast<int>(std::lround(q)), 1, 31);
  }

  void update_rate_control(std::size_t bits) {
    if (bitrate_ <= 0) return;
    const double target = bitrate_ / fps_;
    rc_debt_ += static_cast<double>(bits) - target;
    const double window = target * 8;
    const double ratio = std::clamp(1.0 + rc_debt_ / window, 0.5, 2.0);
    rc_q_ = std::clamp(rc_q_ * std::pow(ratio, 0.25), 1.0, 31.0);
  }

  // ---- headers -------------------------------------------------------------------

  static void start_code(BitWriter& w, std::uint8_t code) {
    w.align();
    w.put(24, 1);
    w.put(8, code);
  }

  [[nodiscard]] std::vector<std::uint8_t> build_sequence_header() const {
    BitWriter w;
    start_code(w, kSequenceHeader);
    w.put(12, static_cast<std::uint32_t>(width_) & 0xFFF);
    w.put(12, static_cast<std::uint32_t>(height_) & 0xFFF);
    w.put(4, 1);  // square samples
    w.put(4, static_cast<std::uint32_t>(frame_rate_code_));
    const std::uint32_t rate400 = bitrate_ > 0 ? static_cast<std::uint32_t>(std::min(bitrate_ * 1.5 / 400.0, 262142.0)) : 0x3FFFF;
    w.put(18, rate400 & 0x3FFFF);
    w.put(1, 1);
    w.put(10, mpeg2_ ? 112 : 20);  // vbv_buffer_size (16 kbit units)
    w.put(1, 0);                   // constrained_parameters_flag
    w.put(1, 0);                   // default intra matrix
    w.put(1, 0);                   // default non-intra matrix
    if (mpeg2_) {
      start_code(w, kExtension);
      w.put(4, 1);  // sequence extension
      const std::uint64_t samples = static_cast<std::uint64_t>(width_) * static_cast<std::uint64_t>(height_);
      const std::uint32_t level = (width_ <= 720 && height_ <= 576) ? 8 : (width_ <= 1440 && samples <= 1440ull * 1152) ? 6 : 4;
      w.put(8, (4u << 4) | level);  // Main profile
      w.put(1, 1);                  // progressive_sequence
      w.put(2, 1);                  // 4:2:0
      w.put(2, (static_cast<std::uint32_t>(width_) >> 12) & 3);
      w.put(2, (static_cast<std::uint32_t>(height_) >> 12) & 3);
      w.put(12, 0);  // bit_rate_extension
      w.put(1, 1);
      w.put(8, 0);  // vbv_buffer_size_extension
      w.put(1, b_frames_ == 0 ? 1 : 0);  // low_delay
      w.put(2, static_cast<std::uint32_t>(frame_rate_n_));
      w.put(5, static_cast<std::uint32_t>(frame_rate_d_));
      start_code(w, kExtension);
      w.put(4, 2);  // sequence display extension
      w.put(3, 5);  // video_format: unspecified
      w.put(1, 1);  // colour_description
      const std::uint32_t mc = color_.matrix == ir::ColorMatrix::BT709 ? 1 : (color_.matrix == ir::ColorMatrix::BT601 ? 6 : 9);
      const std::uint32_t prim = mc == 1 ? 1 : (mc == 6 ? 6 : 9);
      w.put(8, prim);
      w.put(8, mc == 1 ? 1 : (mc == 6 ? 6 : 14));
      w.put(8, mc);
      w.put(14, static_cast<std::uint32_t>(width_) & 0x3FFF);
      w.put(1, 1);
      w.put(14, static_cast<std::uint32_t>(height_) & 0x3FFF);
    }
    w.align();
    return w.take();
  }

  void gop_header(BitWriter& w, std::uint64_t first_display, bool closed) const {
    start_code(w, kGroupStart);
    const auto frames = static_cast<std::uint64_t>(first_display);
    const auto fps_int = static_cast<std::uint64_t>(std::max(1.0, std::round(fps_)));
    const std::uint64_t seconds = frames / fps_int;
    w.put(1, 0);  // drop_frame_flag
    w.put(5, static_cast<std::uint32_t>((seconds / 3600) % 24));
    w.put(6, static_cast<std::uint32_t>((seconds / 60) % 60));
    w.put(1, 1);
    w.put(6, static_cast<std::uint32_t>(seconds % 60));
    w.put(6, static_cast<std::uint32_t>(frames % fps_int));
    w.put(1, closed ? 1 : 0);
    w.put(1, 0);  // broken_link
  }

  void picture_header(BitWriter& w, int type, int temporal_reference) const {
    start_code(w, kPictureStart);
    w.put(10, static_cast<std::uint32_t>(temporal_reference) & 0x3FF);
    w.put(3, static_cast<std::uint32_t>(type));
    w.put(16, 0xFFFF);  // vbv_delay: variable bit rate
    if (type == kP || type == kB) {
      w.put(1, 0);
      w.put(3, mpeg2_ ? 7 : static_cast<std::uint32_t>(f_code_));
    }
    if (type == kB) {
      w.put(1, 0);
      w.put(3, mpeg2_ ? 7 : static_cast<std::uint32_t>(f_code_));
    }
    w.put(1, 0);  // extra_bit_picture
    if (mpeg2_) {
      start_code(w, kExtension);
      w.put(4, 8);
      const std::uint32_t f = static_cast<std::uint32_t>(f_code_);
      w.put(4, type == kI ? 15 : f);
      w.put(4, type == kI ? 15 : f);
      w.put(4, type == kB ? f : 15);
      w.put(4, type == kB ? f : 15);
      w.put(2, 0);  // intra_dc_precision 8 bit
      w.put(2, 3);  // frame picture
      w.put(1, 0);  // top_field_first
      w.put(1, 1);  // frame_pred_frame_dct
      w.put(1, 0);  // concealment_motion_vectors
      w.put(1, 0);  // q_scale_type: linear
      w.put(1, 1);  // intra_vlc_format: table B-15
      w.put(1, 0);  // alternate_scan
      w.put(1, 0);  // repeat_first_field
      w.put(1, 1);  // chroma_420_type
      w.put(1, 1);  // progressive_frame
      w.put(1, 0);  // composite_display_flag
    }
  }

  // ---- picture scheduling ------------------------------------------------------------

  Result<void> encode_anchor_and_bs(InputFrame anchor, int type) {
    // The anchor first (decode order), then the B pictures that precede it in display order.
    const bool new_gop = type == kI;
    std::uint64_t gop_first = anchor.display_index;
    if (new_gop) {
      if (!pending_b_.empty()) gop_first = pending_b_.front().display_index;
      gop_first_display_ = gop_first;
    }
    encode_picture(anchor, type, new_gop, new_gop && pending_b_.empty() && encoded_any_ == false);
    // The anchor becomes the backward reference; the previous one the forward reference.
    for (InputFrame& b : pending_b_) encode_picture(b, kB, false, false);
    pending_b_.clear();
    return {};
  }

  void encode_picture(const InputFrame& frame, int type, bool with_headers, bool closed_gop) {
    BitWriter w;
    if (with_headers) {
      w.put_bytes(sequence_header_);
      gop_header(w, gop_first_display_, closed_gop || !encoded_any_);
    }
    const int temporal_reference = static_cast<int>(frame.display_index - gop_first_display_);
    picture_header(w, type, temporal_reference);
    if (type != kB) {
      // References shift: the previous anchor becomes the forward reference.
      std::swap(ref_fwd_, ref_bwd_);
    }
    const int q = picture_q(type);
    Planes& target = type == kB ? recon_ : ref_bwd_;
    for (int my = 0; my < mb_height_; ++my) encode_slice(w, frame.planes, target, type, q, my);
    std::vector<std::uint8_t> bytes = w.take();
    update_rate_control(bytes.size() * 8);

    Packet packet;
    packet.bytes = to_cvt_bytes(bytes);
    packet.pts = ms_to_ticks(frame.pts_ms, params_.timescale);
    // Decode timestamps: the k-th packet decodes at the k-th display time,
    // shifted by one frame when B pictures delay the output.
    const std::int64_t frame_ms = frame.duration_ms > 0 ? frame.duration_ms : static_cast<std::int64_t>(std::lround(1000.0 / fps_));
    std::int64_t dts_ms = display_pts_.empty() ? frame.pts_ms : display_pts_.front();
    if (!display_pts_.empty()) display_pts_.pop_front();
    if (b_frames_ > 0) dts_ms -= frame_ms;
    packet.dts = std::min(ms_to_ticks(dts_ms, params_.timescale), packet.pts);
    packet.duration = frame.duration_ms > 0 ? ms_to_ticks(frame.duration_ms, params_.timescale) : 0;
    packet.keyframe = type == kI;
    ready_.push_back(std::move(packet));
    encoded_any_ = true;
  }

  // ---- macroblock coding -------------------------------------------------------------

  struct MbChoice {
    int flags = 0;   // kMb* of the coded macroblock
    Vec fwd;
    Vec bwd;
  };

  /// Intra cost estimate: sum of absolute deviations from the block means.
  [[nodiscard]] static int intra_cost(const Planes& src, int mx, int my) {
    int cost = 0;
    for (int b = 0; b < 4; ++b) {
      const std::uint8_t* s = src.at(0, mx * 16 + (b & 1) * 8, my * 16 + (b >> 1) * 8);
      int sum = 0;
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) sum += s[y * src.stride[0] + x];
      }
      const int mean = (sum + 32) / 64;
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) cost += std::abs(s[y * src.stride[0] + x] - mean);
      }
    }
    return cost;
  }

  /// SAD of the 16x16 luma block against the prediction at half-sample vector v.
  [[nodiscard]] int luma_sad(const Planes& src, const Planes& ref, int mx, int my, Vec v) const {
    std::uint8_t pred[256];
    predict(pred, 16, ref.ref(0), mx * 32 + v.x, my * 32 + v.y, 16, 16, false);
    return sad_block(src.at(0, mx * 16, my * 16), src.stride[0], pred, 16, 16, 16);
  }

  [[nodiscard]] Vec clamp_vector(Vec v, int mx, int my) const {
    const int limit = 16 * (1 << (f_code_ - 1));
    const int min_x = std::max(-mx * 32, -limit);
    const int max_x = std::min((mb_width_ - 1 - mx) * 32, limit - 1);
    const int min_y = std::max(-my * 32, -limit);
    const int max_y = std::min((mb_height_ - 1 - my) * 32, limit - 1);
    return {std::clamp(v.x, min_x, max_x), std::clamp(v.y, min_y, max_y)};
  }

  /// Motion search: best predictor, diamond search at full-sample, then half-sample refinement.
  Vec search(const Planes& src, const Planes& ref, int mx, int my, std::initializer_list<Vec> candidates, int& best_sad) const {
    Vec best{};
    best_sad = INT32_MAX;
    for (Vec c : candidates) {
      c = clamp_vector({c.x & ~1, c.y & ~1}, mx, my);
      const int s = luma_sad(src, ref, mx, my, c);
      if (s < best_sad) {
        best_sad = s;
        best = c;
      }
    }
    // Large diamond then small diamond, full-sample steps (2 half-samples).
    const int range = search_range_ * 2;
    for (int step = 8; step >= 2; step /= 2) {
      bool improved = true;
      int iterations = 0;
      while (improved && iterations++ < 16) {
        improved = false;
        const Vec around = best;
        for (const Vec d : {Vec{step, 0}, Vec{-step, 0}, Vec{0, step}, Vec{0, -step}}) {
          Vec c = clamp_vector({around.x + d.x, around.y + d.y}, mx, my);
          if (std::abs(c.x) > range || std::abs(c.y) > range || c == around) continue;
          const int s = luma_sad(src, ref, mx, my, c);
          if (s < best_sad) {
            best_sad = s;
            best = c;
            improved = true;
          }
        }
      }
    }
    // Half-sample refinement.
    const Vec center = best;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        const Vec c = clamp_vector({center.x + dx, center.y + dy}, mx, my);
        if (c == center) continue;
        const int s = luma_sad(src, ref, mx, my, c);
        if (s < best_sad) {
          best_sad = s;
          best = c;
        }
      }
    }
    return best;
  }

  /// Builds the prediction of all planes for a choice into `pred`.
  void build_prediction(const MbChoice& choice, int mx, int my, std::array<std::uint8_t, 384>& pred) const {
    std::uint8_t* planes[3] = {pred.data(), pred.data() + 256, pred.data() + 320};
    const int widths[3] = {16, 8, 8};
    bool have = false;
    for (int dir = 0; dir < 2; ++dir) {
      if ((choice.flags & (dir == 0 ? kMbForward : kMbBackward)) == 0) continue;
      const Planes& ref = dir == 0 ? ref_fwd_ : ref_bwd_;
      const Vec v = dir == 0 ? choice.fwd : choice.bwd;
      for (int p = 0; p < 3; ++p) {
        const int cvx = p == 0 ? v.x : v.x / 2;
        const int cvy = p == 0 ? v.y : v.y / 2;
        const int bx = p == 0 ? mx * 16 : mx * 8;
        const int by = p == 0 ? my * 16 : my * 8;
        predict(planes[p], widths[p], ref.ref(p), bx * 2 + cvx, by * 2 + cvy, widths[p], widths[p], have);
      }
      have = true;
    }
    if (!have) {  // P macroblock without motion compensation: zero vector from the forward reference
      for (int p = 0; p < 3; ++p) {
        const int bx = p == 0 ? mx * 16 : mx * 8;
        const int by = p == 0 ? my * 16 : my * 8;
        predict(planes[p], widths[p], ref_fwd_.ref(p), bx * 2, by * 2, widths[p], widths[p], false);
      }
    }
  }

  /// Quantises one 8x8 block (samples or residual), returns the levels in
  /// raster order and whether any is non-zero; reconstructs the dequantised
  /// coefficients into `recon` (with MPEG-2 mismatch control).
  bool quantise(const std::int32_t* samples, bool intra, int q, std::array<std::int32_t, 64>& levels,
                std::array<std::int32_t, 64>& recon) const {
    std::int32_t coeff[64];
    fdct8x8(samples, coeff);
    const int qscale = mpeg2_ ? 2 * q : q;
    bool any = false;
    std::int64_t sum = 0;
    for (int i = 0; i < 64; ++i) {
      const int w = intra ? kDefaultIntraMatrix[i] : 16;
      int level;
      if (intra && i == 0) {
        level = std::clamp((coeff[0] + 4) >> 3, 0, 255);  // 8-bit DC
        levels[0] = level;
        recon[0] = level * 8;
        sum += recon[0];
        continue;
      }
      // Step size: w * qscale / 16 (MPEG-2) == w * q / 8 (MPEG-1).
      const std::int64_t num = static_cast<std::int64_t>(std::abs(coeff[i])) * 16;
      const std::int64_t den = static_cast<std::int64_t>(w) * qscale * (mpeg2_ ? 1 : 2);
      if (intra) {
        level = static_cast<int>((num * 8 + den * 3) / (den * 8));  // rounding with a small dead zone
      } else {
        level = static_cast<int>((num * 4 + den) / (den * 4));      // dead zone towards zero
      }
      level = std::min(level, mpeg2_ ? 2047 : 255);
      if (coeff[i] < 0) level = -level;
      levels[static_cast<std::size_t>(i)] = level;
      if (level != 0) {
        any = true;
        recon[static_cast<std::size_t>(i)] = intra ? dequant_intra(mpeg2_, level, w, mpeg2_ ? qscale : q)
                                                   : dequant_inter(mpeg2_, level, w, mpeg2_ ? qscale : q);
      } else {
        recon[static_cast<std::size_t>(i)] = 0;
      }
      sum += recon[static_cast<std::size_t>(i)];
    }
    if (mpeg2_ && (any || intra) && (sum & 1) == 0) recon[63] ^= 1;
    return any;
  }

  void put_motion_component(BitWriter& w, int value, int& pred) const {
    const int r_size = f_code_ - 1;
    const int f = 1 << r_size;
    int delta = value - pred;
    if (delta < -16 * f) delta += 32 * f;
    if (delta > 16 * f - 1) delta -= 32 * f;
    pred = value;
    if (delta == 0) {
      w.put(1, 1);
      return;
    }
    const int magnitude = std::abs(delta) - 1;
    const int motion_code = (magnitude >> r_size) + 1;
    const Code c = find_code(kMotionCodes, motion_code);
    w.put(c.length, c.bits);
    w.put(1, delta < 0 ? 1 : 0);
    if (r_size > 0) w.put(r_size, static_cast<std::uint32_t>(magnitude & (f - 1)));
  }

  void put_block(BitWriter& w, const std::array<std::int32_t, 64>& levels, bool intra, int comp, int& dc_pred) const {
    int start = 0;
    const DctCodes& table = (intra && mpeg2_) ? dct_one_ : dct_zero_;
    if (intra) {
      const int diff = levels[0] - dc_pred;
      dc_pred = levels[0];
      const int magnitude = std::abs(diff);
      int size = 0;
      while ((1 << size) <= magnitude) ++size;
      const Code c = find_code(comp == 0 ? kDcLumaCodes : kDcChromaCodes, size);
      w.put(c.length, c.bits);
      if (size > 0) w.put(size, static_cast<std::uint32_t>(diff > 0 ? diff : diff + (1 << size) - 1));
      start = 1;
    }
    int run = 0;
    bool first = !intra;
    for (int i = start; i < 64; ++i) {
      const int level = levels[zigzag[i]];
      if (level == 0) {
        ++run;
        continue;
      }
      const int magnitude = std::abs(level);
      if (first && run == 0 && magnitude == 1) {
        w.put(1, 1);
        w.put(1, level < 0 ? 1 : 0);
      } else if (run < 32 && magnitude <= 40 && table.rl[static_cast<std::size_t>(run)][static_cast<std::size_t>(magnitude)].length > 0) {
        const Code c = table.rl[static_cast<std::size_t>(run)][static_cast<std::size_t>(magnitude)];
        w.put(c.length, c.bits);
        w.put(1, level < 0 ? 1 : 0);
      } else {
        w.put(6, 1);  // escape
        w.put(6, static_cast<std::uint32_t>(run));
        if (mpeg2_) {
          w.put_signed(12, level);
        } else if (magnitude < 128) {
          w.put_signed(8, level);
        } else if (level > 0) {
          w.put(8, 0);
          w.put(8, static_cast<std::uint32_t>(level));
        } else {
          w.put(8, 0x80);
          w.put(8, static_cast<std::uint32_t>(level + 256));
        }
      }
      first = false;
      run = 0;
    }
    w.put(table.eob.length, table.eob.bits);
  }

  void encode_slice(BitWriter& w, const Planes& src, Planes& out, int type, int q, int my) {
    start_code(w, static_cast<std::uint8_t>(my + 1));
    w.put(5, static_cast<std::uint32_t>(q));
    w.put(1, 0);  // extra_bit_slice
    int dc_pred[3] = {128, 128, 128};
    int pmv[2][2] = {{0, 0}, {0, 0}};  // [s][t]
    int last_coded = -1;
    MbChoice previous;
    bool previous_intra = true;
    const Code mba_escape = find_code(kMbaCodes, kMbaEscape);
    for (int mx = 0; mx < mb_width_; ++mx) {
      // ---- mode decision ----
      MbChoice choice;
      const int idx = my * mb_width_ + mx;
      if (type == kI) {
        choice.flags = kMbIntra;
      } else {
        const int icost = intra_cost(src, mx, my) + 500;
        const Vec left = mx > 0 ? mvs_[static_cast<std::size_t>(idx - 1)] : Vec{};
        const Vec top = my > 0 ? mvs_[static_cast<std::size_t>(idx - mb_width_)] : Vec{};
        const Vec same = mvs_[static_cast<std::size_t>(idx)];
        int fsad = 0;
        const Vec fwd = search(src, ref_fwd_, mx, my, {Vec{}, left, top, same}, fsad);
        if (type == kP) {
          const int zero_sad = luma_sad(src, ref_fwd_, mx, my, Vec{});
          if (zero_sad <= fsad + 64) {
            choice.flags = 0;  // no motion compensation (zero vector)
            fsad = zero_sad;
          } else {
            choice.flags = kMbForward;
            choice.fwd = fwd;
          }
          if (icost < fsad) choice.flags = kMbIntra;
          mvs_[static_cast<std::size_t>(idx)] = (choice.flags & kMbForward) != 0 ? fwd : Vec{};
        } else {
          int bsad = 0;
          const Vec bwd = search(src, ref_bwd_, mx, my, {Vec{}, previous.bwd}, bsad);
          std::uint8_t a[256];
          std::uint8_t b[256];
          predict(a, 16, ref_fwd_.ref(0), mx * 32 + fwd.x, my * 32 + fwd.y, 16, 16, false);
          std::memcpy(b, a, sizeof(a));
          predict(b, 16, ref_bwd_.ref(0), mx * 32 + bwd.x, my * 32 + bwd.y, 16, 16, true);
          const int bisad = sad_block(src.at(0, mx * 16, my * 16), src.stride[0], b, 16, 16, 16);
          choice.fwd = fwd;
          choice.bwd = bwd;
          if (bisad <= fsad && bisad <= bsad) choice.flags = kMbForward | kMbBackward;
          else if (fsad <= bsad) choice.flags = kMbForward;
          else choice.flags = kMbBackward;
          const int best = std::min({bisad, fsad, bsad});
          if (icost < best) choice.flags = kMbIntra;
        }
      }
      const bool intra = (choice.flags & kMbIntra) != 0;

      // ---- transform and quantisation ----
      std::array<std::uint8_t, 384> pred{};
      if (!intra) build_prediction(choice, mx, my, pred);
      std::array<std::array<std::int32_t, 64>, 6> levels{};
      std::array<std::array<std::int32_t, 64>, 6> recon{};
      int cbp = 0;
      for (int b = 0; b < 6; ++b) {
        std::int32_t samples[64];
        const int p = b < 4 ? 0 : b - 3;
        const int bx = p == 0 ? mx * 16 + (b & 1) * 8 : mx * 8;
        const int by = p == 0 ? my * 16 + (b >> 1) * 8 : my * 8;
        const std::uint8_t* s = src.at(p, bx, by);
        const std::uint8_t* pr = p == 0 ? pred.data() + (b >> 1) * 8 * 16 + (b & 1) * 8 : pred.data() + 256 + (p - 1) * 64;
        const int pstride = p == 0 ? 16 : 8;
        for (int y = 0; y < 8; ++y) {
          for (int x = 0; x < 8; ++x) {
            const int v = s[y * src.stride[static_cast<std::size_t>(p)] + x];
            samples[y * 8 + x] = intra ? v : v - pr[y * pstride + x];
          }
        }
        if (quantise(samples, intra, q, levels[static_cast<std::size_t>(b)], recon[static_cast<std::size_t>(b)])) {
          cbp |= 1 << (5 - b);
        }
      }

      // ---- skipping ----
      const bool last_in_row = mx == mb_width_ - 1;
      bool skip = false;
      if (!intra && cbp == 0 && mx != 0 && !last_in_row) {
        if (type == kP && choice.flags == 0) skip = true;
        if (type == kB && !previous_intra && choice.flags == previous.flags &&
            ((choice.flags & kMbForward) == 0 || choice.fwd == previous.fwd) &&
            ((choice.flags & kMbBackward) == 0 || choice.bwd == previous.bwd)) {
          skip = true;
        }
      }
      if (skip) {
        if (type == kP) pmv[0][0] = pmv[0][1] = 0;
        reconstruct(out, pred, recon, 0, false, mx, my);
        dc_pred[0] = dc_pred[1] = dc_pred[2] = 128;
        continue;
      }

      // ---- macroblock header ----
      int increment = mx - last_coded;
      while (increment > 33) {
        w.put(mba_escape.length, mba_escape.bits);
        increment -= 33;
      }
      const Code inc = find_code(kMbaCodes, increment);
      w.put(inc.length, inc.bits);
      last_coded = mx;
      int flags = choice.flags;
      if (!intra && cbp != 0) flags |= kMbPattern;
      if (type == kP && !intra && (flags & kMbForward) == 0 && cbp == 0) {
        // "No MC, not coded" does not exist: send a zero forward vector instead.
        flags |= kMbForward;
        choice.flags |= kMbForward;
        choice.fwd = Vec{};
      }
      const Code mbt = type == kI ? find_code(kMbTypeI, flags) : type == kP ? find_code(kMbTypeP, flags) : find_code(kMbTypeB, flags);
      w.put(mbt.length, mbt.bits);
      if ((flags & kMbForward) != 0) {
        put_motion_component(w, choice.fwd.x, pmv[0][0]);
        put_motion_component(w, choice.fwd.y, pmv[0][1]);
      }
      if ((flags & kMbBackward) != 0) {
        put_motion_component(w, choice.bwd.x, pmv[1][0]);
        put_motion_component(w, choice.bwd.y, pmv[1][1]);
      }
      if (intra) {
        pmv[0][0] = pmv[0][1] = pmv[1][0] = pmv[1][1] = 0;
      } else if (type == kP && (flags & kMbForward) == 0) {
        pmv[0][0] = pmv[0][1] = 0;
      }
      if ((flags & kMbPattern) != 0) {
        const Code c = find_code(kCbpCodes, cbp);
        w.put(c.length, c.bits);
      }
      if (!intra) dc_pred[0] = dc_pred[1] = dc_pred[2] = 128;
      for (int b = 0; b < 6; ++b) {
        if (!intra && (cbp & (1 << (5 - b))) == 0) continue;
        const int comp = b < 4 ? 0 : b - 3;
        put_block(w, levels[static_cast<std::size_t>(b)], intra, comp, dc_pred[comp]);
      }
      reconstruct(out, pred, recon, intra ? 0x3F : cbp, intra, mx, my);
      previous = choice;
      previous_intra = intra;
    }
  }

  static void reconstruct(Planes& out, const std::array<std::uint8_t, 384>& pred,
                          const std::array<std::array<std::int32_t, 64>, 6>& recon, int coded, bool intra, int mx,
                          int my) {
    for (int b = 0; b < 6; ++b) {
      const int p = b < 4 ? 0 : b - 3;
      const int bx = p == 0 ? mx * 16 + (b & 1) * 8 : mx * 8;
      const int by = p == 0 ? my * 16 + (b >> 1) * 8 : my * 8;
      std::uint8_t* d = out.at(p, bx, by);
      const int stride = out.stride[static_cast<std::size_t>(p)];
      if (intra) {
        idct8x8_put(recon[static_cast<std::size_t>(b)].data(), d, stride);
        continue;
      }
      const std::uint8_t* pr = p == 0 ? pred.data() + (b >> 1) * 8 * 16 + (b & 1) * 8 : pred.data() + 256 + (p - 1) * 64;
      const int pstride = p == 0 ? 16 : 8;
      for (int y = 0; y < 8; ++y) std::memcpy(d + y * stride, pr + y * pstride, 8);
      if ((coded & (1 << (5 - b))) != 0) idct8x8_add(recon[static_cast<std::size_t>(b)].data(), d, stride);
    }
  }

  bool mpeg2_;
  VideoEncodeParams params_;
  ColorInfo color_;
  int width_ = 0;
  int height_ = 0;
  int mb_width_ = 0;
  int mb_height_ = 0;
  int frame_rate_code_ = 3;
  int frame_rate_n_ = 0;
  int frame_rate_d_ = 0;
  double fps_ = 25.0;
  int gop_ = 12;
  int b_frames_ = 2;
  int search_range_ = 16;
  int f_code_ = 3;
  int base_q_ = 3;
  double bitrate_ = 0;
  double rc_q_ = 6;
  double rc_debt_ = 0;

  DctCodes dct_zero_{kDctZero};
  DctCodes dct_one_{kDctOne};
  std::vector<std::uint8_t> sequence_header_;
  Planes ref_fwd_;
  Planes ref_bwd_;
  Planes recon_;
  std::vector<Vec> mvs_;
  std::deque<InputFrame> pending_b_;
  std::deque<std::int64_t> display_pts_;
  std::deque<Packet> ready_;
  std::uint64_t input_count_ = 0;
  std::uint64_t gop_first_display_ = 0;
  bool encoded_any_ = false;
};

}  // namespace

std::unique_ptr<codec::VideoEncoder> make_encoder(bool mpeg2) { return std::make_unique<Mpeg12Encoder>(mpeg2); }

}  // namespace cvt::legacy::mpeg12
