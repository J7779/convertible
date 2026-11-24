// Convertible core - codec_legacy family: MPEG-4 Part 2 Simple Profile,
// H.263 and Sorenson Spark video encoder.
//
// I and P pictures with half-sample motion estimation (one vector per
// macroblock), H.263 quantisation, intra DC prediction (MPEG-4), skipped
// macroblocks. MPEG-4 streams carry VOS/VO/VOL headers both out of band
// (extradata) and in band before every I-VOP. H.263 uses the baseline
// picture header for the standard sizes and PLUSPTYPE with a custom picture
// format otherwise (sizes must then be multiples of 4). Constant quality from
// `quality`, or a per-picture rate control from `bitrate_kbps`.
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
#include "families/codec_legacy/mpeg4/mpeg4.hpp"
#include "families/codec_legacy/mpeg4/mpeg4_common.hpp"
#include "families/codec_legacy/mpeg4/mpeg4_tables.hpp"

namespace cvt::legacy::mpeg4 {

namespace {

struct Code {
  std::uint32_t bits = 0;
  int length = 0;
};

template <std::size_t N>
[[nodiscard]] Code find(const VlcCode (&table)[N], int symbol) {
  for (const VlcCode& c : table) {
    if (c.symbol == symbol) return {c.code, c.length};
  }
  return {};
}

/// (last, run, level) -> code for one TCOEF table.
struct TcoefCodes {
  std::array<std::array<std::array<Code, 28>, 64>, 2> code{};
  template <std::size_t N>
  explicit TcoefCodes(const VlcCode (&table)[N]) {
    for (const VlcCode& c : table) {
      if (c.symbol == kEscape) continue;
      code[static_cast<std::size_t>(c.symbol >> 12)][static_cast<std::size_t>((c.symbol >> 6) & 63)]
          [static_cast<std::size_t>(c.symbol & 63)] = {c.code, c.length};
    }
  }
  [[nodiscard]] const Code* lookup(int last, int run, int level) const {
    if (run > 63 || level > 27) return nullptr;
    const Code& c = code[static_cast<std::size_t>(last)][static_cast<std::size_t>(run)][static_cast<std::size_t>(level)];
    return c.length > 0 ? &c : nullptr;
  }
};

struct Vec {
  int x = 0;
  int y = 0;
  friend bool operator==(const Vec&, const Vec&) = default;
};

[[nodiscard]] int sad16(const std::uint8_t* a, int as, const std::uint8_t* b, int bs) {
  int sum = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) sum += std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x]));
    a += as;
    b += bs;
  }
  return sum;
}

class Mpeg4Encoder final : public codec::VideoEncoder {
 public:
  explicit Mpeg4Encoder(Flavor flavor) : flavor_(flavor) {}

  Result<void> configure(const VideoEncodeParams& params) override {
    if (params.width == 0 || params.height == 0) return fail(Status::InvalidArgument, "the picture has no pixels");
    width_ = static_cast<int>(params.width);
    height_ = static_cast<int>(params.height);
    if (flavor_ == Flavor::H263) {
      h263_format_ = 0;
      static constexpr int kW[6] = {0, 128, 176, 352, 704, 1408};
      static constexpr int kH[6] = {0, 96, 144, 288, 576, 1152};
      for (int f = 1; f <= 5; ++f) {
        if (kW[f] == width_ && kH[f] == height_) h263_format_ = f;
      }
      if (h263_format_ == 0 && (width_ % 4 != 0 || height_ % 4 != 0 || width_ > 2048 || height_ > 1152)) {
        return fail(Status::InvalidArgument,
                    "H.263 needs a standard picture size or a custom size up to 2048x1152 in multiples of 4 (got {}x{})",
                    width_, height_);
      }
    } else if (flavor_ == Flavor::Flv1) {
      if (width_ > 65535 || height_ > 65535) return fail(Status::InvalidArgument, "picture too large for Sorenson Spark");
    } else if (width_ > 8191 || height_ > 8191) {
      return fail(Status::InvalidArgument, "picture too large for MPEG-4 Part 2");
    }
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 90000;
    mb_width_ = (width_ + 15) / 16;
    mb_height_ = (height_ + 15) / 16;
    gop_ = params.keyframe_interval > 0 ? static_cast<int>(std::min<std::uint32_t>(params.keyframe_interval, 600)) : 250;
    search_range_ = params.preset == EncodePreset::Small ? 15 : (params.preset == EncodePreset::Fast ? 7 : 15);
    base_q_ = std::clamp(static_cast<int>(std::lround(quality_to_scale(params.quality, 1.0, 2.0, 31.0))), 1, 31);
    if (params.bitrate_kbps) {
      bitrate_ = static_cast<double>(*params.bitrate_kbps) * 1000.0;
      rc_q_ = 8.0;
    }
    fps_ = params.fps.value() > 0 ? params.fps.value() : 25.0;
    // VOP time resolution: the nominal frame rate (an integer), at least 1.
    time_resolution_ = std::clamp(static_cast<int>(std::lround(params.fps.num > 0 && params.fps.den > 0 ? (params.fps.num % params.fps.den == 0 ? fps_ : params.fps.num) : 25)), 1, 65535);
    time_increment_ = params.fps.num > 0 && params.fps.den > 0 && params.fps.num % params.fps.den != 0
                          ? static_cast<int>(std::clamp<std::int64_t>(params.fps.den, 1, 65535))
                          : 1;
    time_bits_ = 1;
    while ((1 << time_bits_) < time_resolution_) ++time_bits_;
    color_ = params.color;
    color_.range = ir::ColorRange::Limited;
    color_.siting = flavor_ == Flavor::Mpeg4 ? ir::ChromaSiting::Left : ir::ChromaSiting::Center;
    if (flavor_ != Flavor::Mpeg4) color_.matrix = ir::ColorMatrix::BT601;
    reference_.allocate(mb_width_ * 16, mb_height_ * 16);
    current_.allocate(mb_width_ * 16, mb_height_ * 16);
    source_.allocate(mb_width_ * 16, mb_height_ * 16);
    mvs_.assign(static_cast<std::size_t>(mb_width_ * mb_height_), Vec{});
    if (flavor_ == Flavor::Mpeg4) headers_ = build_headers();
    return {};
  }

  Result<void> send_frame(const VideoFrame& frame) override {
    if (frame.width != params_.width || frame.height != params_.height) {
      return fail(Status::InvalidArgument, "frame size {}x{} differs from the configured {}x{}", frame.width,
                  frame.height, params_.width, params_.height);
    }
    CVT_TRY_ASSIGN(const VideoFrame yuv, to_format(frame, PixelFormat::I420, color_));
    for (int p = 0; p < 3; ++p) {
      const int w = static_cast<int>(ir::plane_width(PixelFormat::I420, static_cast<std::size_t>(p), params_.width));
      const int h = static_cast<int>(ir::plane_height(PixelFormat::I420, static_cast<std::size_t>(p), params_.height));
      const auto& src = yuv.planes[static_cast<std::size_t>(p)];
      for (int y = 0; y < source_.rows[static_cast<std::size_t>(p)]; ++y) {
        const std::uint8_t* s = src.data.data() + static_cast<std::size_t>(std::min(y, h - 1)) * src.stride;
        std::uint8_t* d = source_.at(p, 0, y);
        std::memcpy(d, s, static_cast<std::size_t>(w));
        for (int x = w; x < source_.stride[static_cast<std::size_t>(p)]; ++x) d[x] = s[w - 1];
      }
    }
    const bool intra = frame_index_ % static_cast<std::uint64_t>(gop_) == 0;
    const int q = picture_q(intra);
    BitWriter w;
    if (flavor_ == Flavor::Mpeg4) {
      if (intra) w.put_bytes(headers_);
      vop_header(w, intra, q);
    } else if (flavor_ == Flavor::H263) {
      h263_header(w, intra, q);
    } else {
      flv_header(w, intra, q);
    }
    encode_picture(w, intra, q);
    if (flavor_ == Flavor::Mpeg4) {
      // next_start_code(): a zero bit then ones up to the byte boundary.
      w.put(1, 0);
      while (!w.byte_aligned()) w.put(1, 1);
    } else {
      w.align();
    }
    std::vector<std::uint8_t> bytes = w.take();
    update_rate_control(bytes.size() * 8);
    std::swap(reference_, current_);

    Packet packet;
    packet.bytes = to_cvt_bytes(bytes);
    packet.pts = ms_to_ticks(frame.pts_ms, params_.timescale);
    packet.dts = packet.pts;
    packet.duration = frame.duration_ms > 0 ? ms_to_ticks(frame.duration_ms, params_.timescale) : 0;
    packet.keyframe = intra;
    ready_.push_back(std::move(packet));
    ++frame_index_;
    return {};
  }

  Result<std::optional<Packet>> receive_packet() override {
    if (ready_.empty()) return std::optional<Packet>{};
    Packet packet = std::move(ready_.front());
    ready_.pop_front();
    return std::optional<Packet>(std::move(packet));
  }

  Bytes extradata() const override { return to_cvt_bytes(headers_); }
  Result<void> flush() override { return {}; }

 private:
  [[nodiscard]] int picture_q(bool intra) const {
    double q = bitrate_ > 0 ? rc_q_ : static_cast<double>(base_q_);
    if (intra && bitrate_ > 0) q *= 0.8;
    // H.263 escapes carry levels up to 127 only: keep Q >= 3 so that no coefficient clips.
    return std::clamp(static_cast<int>(std::lround(q)), flavor_ == Flavor::H263 ? 3 : 1, 31);
  }

  void update_rate_control(std::size_t bits) {
    if (bitrate_ <= 0) return;
    const double target = bitrate_ / fps_;
    rc_debt_ += static_cast<double>(bits) - target;
    const double ratio = std::clamp(1.0 + rc_debt_ / (target * 8), 0.5, 2.0);
    rc_q_ = std::clamp(rc_q_ * std::pow(ratio, 0.25), 1.0, 31.0);
  }

  // ---- headers -----------------------------------------------------------------------------

  [[nodiscard]] std::vector<std::uint8_t> build_headers() const {
    BitWriter w;
    w.put(32, 0x000001B0);  // visual_object_sequence_start_code
    w.put(8, 0x03);         // Simple Profile @ Level 3
    w.put(32, 0x000001B5);  // visual_object_start_code
    w.put(1, 0);            // is_visual_object_identifier
    w.put(4, 1);            // video ID
    w.put(1, 1);            // video_signal_type
    w.put(3, 5);            // video_format: unspecified
    w.put(1, 0);            // video_range: limited
    w.put(1, 1);            // colour_description
    const std::uint32_t mc = color_.matrix == ir::ColorMatrix::BT709 ? 1 : (color_.matrix == ir::ColorMatrix::BT2020 ? 9 : 6);
    w.put(8, mc == 6 ? 6 : mc);
    w.put(8, mc == 1 ? 1 : (mc == 9 ? 14 : 6));
    w.put(8, mc);
    w.put(1, 0);  // next_start_code stuffing
    while (!w.byte_aligned()) w.put(1, 1);
    w.put(32, 0x00000100);  // video_object_start_code
    w.put(32, 0x00000120);  // video_object_layer_start_code
    w.put(1, 0);            // random_accessible_vol
    w.put(8, 1);            // simple object type
    w.put(1, 0);            // is_object_layer_identifier
    w.put(4, 1);            // square samples
    w.put(1, 1);            // vol_control_parameters
    w.put(2, 1);            // 4:2:0
    w.put(1, 1);            // low_delay
    w.put(1, 0);            // vbv_parameters
    w.put(2, 0);            // rectangular
    w.put(1, 1);
    w.put(16, static_cast<std::uint32_t>(time_resolution_));
    w.put(1, 1);
    w.put(1, 1);  // fixed_vop_rate
    w.put(time_bits_, static_cast<std::uint32_t>(time_increment_));
    w.put(1, 1);
    w.put(13, static_cast<std::uint32_t>(width_));
    w.put(1, 1);
    w.put(13, static_cast<std::uint32_t>(height_));
    w.put(1, 1);
    w.put(1, 0);  // interlaced
    w.put(1, 1);  // obmc_disable
    w.put(1, 0);  // sprite_enable
    w.put(1, 0);  // not_8_bit
    w.put(1, 0);  // quant_type: H.263
    w.put(1, 1);  // complexity_estimation_disable
    w.put(1, 1);  // resync_marker_disable
    w.put(1, 0);  // data_partitioned
    w.put(1, 0);  // scalability
    w.put(1, 0);
    while (!w.byte_aligned()) w.put(1, 1);
    return w.take();
  }

  void vop_header(BitWriter& w, bool intra, int q) {
    w.put(32, 0x000001B6);
    w.put(2, intra ? 0 : 1);
    // modulo_time_base and vop_time_increment from the frame count.
    const std::uint64_t ticks = frame_index_ * static_cast<std::uint64_t>(time_increment_);
    const std::uint64_t seconds = ticks / static_cast<std::uint64_t>(time_resolution_);
    std::uint64_t modulo = seconds - last_second_;
    last_second_ = seconds;
    while (modulo-- > 0) w.put(1, 1);
    w.put(1, 0);
    w.put(1, 1);
    w.put(time_bits_, static_cast<std::uint32_t>(ticks % static_cast<std::uint64_t>(time_resolution_)));
    w.put(1, 1);
    w.put(1, 1);  // vop_coded
    if (!intra) w.put(1, 0);  // vop_rounding_type
    w.put(3, 0);              // intra_dc_vlc_thr: always the DC VLC
    w.put(5, static_cast<std::uint32_t>(q));
    if (!intra) w.put(3, 1);  // vop_fcode_forward
  }

  void h263_header(BitWriter& w, bool intra, int q) {
    w.put(22, 0x20);
    w.put(8, static_cast<std::uint32_t>(frame_index_ & 0xFF));
    w.put(1, 1);
    w.put(1, 0);
    w.put(3, 0);  // split screen, document camera, freeze release
    if (h263_format_ != 0) {
      w.put(3, static_cast<std::uint32_t>(h263_format_));
      w.put(1, intra ? 0 : 1);
      w.put(4, 0);  // UMV, SAC, AP, PB off
      w.put(5, static_cast<std::uint32_t>(q));
      w.put(1, 0);  // CPM
    } else {
      w.put(3, 7);  // extended PTYPE
      w.put(3, 1);  // UFEP: OPPTYPE present
      w.put(3, 6);  // custom source format
      w.put(1, 0);  // custom PCF
      w.put(10, 0);  // UMV, SAC, AP, AIC, DF, SS, RPS, ISD, AIV, MQ off
      w.put(1, 1);
      w.put(3, 0);
      w.put(3, intra ? 0 : 1);
      w.put(2, 0);  // RPR, RRU
      w.put(1, 0);  // rounding type
      w.put(2, 0);
      w.put(1, 1);
      w.put(1, 0);  // CPM
      w.put(4, 1);  // square pixels
      w.put(9, static_cast<std::uint32_t>(width_ / 4 - 1));
      w.put(1, 1);
      w.put(9, static_cast<std::uint32_t>(height_ / 4));
      w.put(5, static_cast<std::uint32_t>(q));
    }
    w.put(1, 0);  // PEI
  }

  void flv_header(BitWriter& w, bool intra, int q) {
    w.put(17, 1);
    w.put(5, 1);  // version 1: 7/11-bit escapes
    w.put(8, static_cast<std::uint32_t>(frame_index_ & 0xFF));
    if (width_ < 256 && height_ < 256) {
      w.put(3, 0);
      w.put(8, static_cast<std::uint32_t>(width_));
      w.put(8, static_cast<std::uint32_t>(height_));
    } else {
      w.put(3, 1);
      w.put(16, static_cast<std::uint32_t>(width_));
      w.put(16, static_cast<std::uint32_t>(height_));
    }
    w.put(2, intra ? 0 : 1);
    w.put(1, 0);  // deblocking hint
    w.put(5, static_cast<std::uint32_t>(q));
    w.put(1, 0);  // PEI
  }

  // ---- macroblocks ------------------------------------------------------------------------------

  [[nodiscard]] Vec clamp_vector(Vec v, int mx, int my) const {
    // Keep the 16x16 block inside the (macroblock-aligned) picture and the fcode-1 range.
    const int min_x = std::max(-mx * 32, -32);
    const int max_x = std::min((mb_width_ - 1 - mx) * 32, 31);
    const int min_y = std::max(-my * 32, -32);
    const int max_y = std::min((mb_height_ - 1 - my) * 32, 31);
    return {std::clamp(v.x, min_x, max_x), std::clamp(v.y, min_y, max_y)};
  }

  [[nodiscard]] int luma_sad(int mx, int my, Vec v) const {
    std::uint8_t pred[256];
    mc_half(pred, 16, view(reference_, 0), mx * 32 + v.x, my * 32 + v.y, 16, 16, 0, false);
    return sad16(source_.at(0, mx * 16, my * 16), source_.stride[0], pred, 16);
  }

  Vec search(int mx, int my, std::initializer_list<Vec> candidates, int& best_sad) const {
    Vec best{};
    best_sad = INT32_MAX;
    for (Vec c : candidates) {
      c = clamp_vector({c.x & ~1, c.y & ~1}, mx, my);
      const int s = luma_sad(mx, my, c);
      if (s < best_sad) {
        best_sad = s;
        best = c;
      }
    }
    for (int step = 8; step >= 2; step /= 2) {
      bool improved = true;
      int iterations = 0;
      while (improved && iterations++ < 16) {
        improved = false;
        const Vec around = best;
        for (const Vec d : {Vec{step, 0}, Vec{-step, 0}, Vec{0, step}, Vec{0, -step}}) {
          const Vec c = clamp_vector({around.x + d.x, around.y + d.y}, mx, my);
          if (c == around || std::abs(c.x) > search_range_ * 2 || std::abs(c.y) > search_range_ * 2) continue;
          const int s = luma_sad(mx, my, c);
          if (s < best_sad) {
            best_sad = s;
            best = c;
            improved = true;
          }
        }
      }
    }
    const Vec center = best;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const Vec c = clamp_vector({center.x + dx, center.y + dy}, mx, my);
        if (c == center) continue;
        const int s = luma_sad(mx, my, c);
        if (s < best_sad) {
          best_sad = s;
          best = c;
        }
      }
    }
    return best;
  }

  [[nodiscard]] int intra_cost(int mx, int my) const {
    int cost = 0;
    for (int b = 0; b < 4; ++b) {
      const std::uint8_t* s = source_.at(0, mx * 16 + (b & 1) * 8, my * 16 + (b >> 1) * 8);
      int sum = 0;
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) sum += s[y * source_.stride[0] + x];
      }
      const int mean = (sum + 32) / 64;
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) cost += std::abs(s[y * source_.stride[0] + x] - mean);
      }
    }
    return cost;
  }

  /// Median motion vector predictor (frame, one vector per macroblock, one packet).
  [[nodiscard]] Vec predictor(int mx, int my) const {
    const auto at = [&](int x, int y) { return mvs_[static_cast<std::size_t>(y * mb_width_ + x)]; };
    const bool has_left = mx > 0;
    const bool has_above = my > 0;
    const bool has_right = my > 0 && mx + 1 < mb_width_;
    Vec c[3] = {has_left ? at(mx - 1, my) : Vec{}, has_above ? at(mx, my - 1) : Vec{}, has_right ? at(mx + 1, my - 1) : Vec{}};
    const bool ok[3] = {has_left, has_above, has_right};
    const int missing = (ok[0] ? 0 : 1) + (ok[1] ? 0 : 1) + (ok[2] ? 0 : 1);
    if (missing == 3) return {};
    if (missing == 2) {
      for (int i = 0; i < 3; ++i) {
        if (ok[i]) return c[i];
      }
    }
    const auto median = [](int a, int b, int d) { return std::max(std::min(a, b), std::min(std::max(a, b), d)); };
    return {median(c[0].x, c[1].x, c[2].x), median(c[0].y, c[1].y, c[2].y)};
  }

  void put_mvd(BitWriter& w, int value, int pred) const {
    int diff = value - pred;
    if (diff < -32) diff += 64;
    if (diff > 31) diff -= 64;
    const Code c = find(kMvd, std::abs(diff));
    w.put(c.length, c.bits);
    if (diff != 0) w.put(1, diff < 0 ? 1 : 0);
  }

  /// Codes TCOEF events of `levels` (raster) along zigzag from `start`.
  void put_coefficients(BitWriter& w, const std::array<int, 64>& levels, int start, bool intra_table) const {
    int last_index = -1;
    for (int i = 63; i >= start; --i) {
      if (levels[zigzag[i]] != 0) {
        last_index = i;
        break;
      }
    }
    int run = 0;
    const TcoefCodes& table = intra_table ? intra_codes_ : inter_codes_;
    for (int i = start; i <= last_index; ++i) {
      const int level = levels[zigzag[i]];
      if (level == 0) {
        ++run;
        continue;
      }
      const int last = i == last_index ? 1 : 0;
      const int magnitude = std::abs(level);
      if (const Code* c = table.lookup(last, run, magnitude)) {
        w.put(c->length, c->bits);
        w.put(1, level < 0 ? 1 : 0);
      } else {
        w.put(7, 3);  // escape 0000011
        if (flavor_ == Flavor::Mpeg4) {
          w.put(2, 3);  // type 3: fixed length
          w.put(1, static_cast<std::uint32_t>(last));
          w.put(6, static_cast<std::uint32_t>(run));
          w.put(1, 1);
          w.put_signed(12, level);
          w.put(1, 1);
        } else if (flavor_ == Flavor::Flv1) {
          const bool wide = magnitude > 63;
          w.put(1, wide ? 1 : 0);
          w.put(1, static_cast<std::uint32_t>(last));
          w.put(6, static_cast<std::uint32_t>(run));
          w.put_signed(wide ? 11 : 7, level);
        } else {
          w.put(1, static_cast<std::uint32_t>(last));
          w.put(6, static_cast<std::uint32_t>(run));
          w.put_signed(8, level);
        }
      }
      run = 0;
    }
  }

  /// Largest level the escape of this syntax can carry.
  [[nodiscard]] int max_level() const {
    if (flavor_ == Flavor::Mpeg4) return 2047;
    if (flavor_ == Flavor::Flv1) return 1023;
    return 127;
  }

  /// H.263 quantisation of one 8x8 block of samples or residuals; fills the
  /// levels and the reconstructed (dequantised) coefficients.
  bool quantise(const std::int32_t* samples, bool intra, int q, std::array<int, 64>& levels,
                std::array<std::int32_t, 64>& recon, int dc_scale) const {
    std::int32_t coeff[64];
    fdct8x8(samples, coeff);
    bool any = false;
    for (int i = 0; i < 64; ++i) {
      if (intra && i == 0) {
        const int dc = (coeff[0] + dc_scale / 2) / dc_scale;
        levels[0] = flavor_ == Flavor::Mpeg4 ? std::clamp(dc, 0, 2047 / dc_scale) : std::clamp(dc, 1, 254);
        recon[0] = levels[0] * dc_scale;
        continue;
      }
      const int magnitude = std::abs(coeff[i]);
      int level = intra ? magnitude / (2 * q) : std::max(0, magnitude - q / 2) / (2 * q);
      level = std::min(level, max_level());
      if (coeff[i] < 0) level = -level;
      levels[static_cast<std::size_t>(i)] = level;
      recon[static_cast<std::size_t>(i)] = dequant_h263(level, q);
      any = any || level != 0;
    }
    return any;
  }

  void encode_picture(BitWriter& w, bool intra_picture, int q) {
    // DC predictors (MPEG-4): per 8x8 block, value 1024 = unavailable / not intra.
    std::vector<int> dc_luma(static_cast<std::size_t>(mb_width_ * 2 * mb_height_ * 2), -1);
    std::vector<int> dc_chroma(static_cast<std::size_t>(mb_width_ * mb_height_ * 2), -1);
    for (int my = 0; my < mb_height_; ++my) {
      for (int mx = 0; mx < mb_width_; ++mx) {
        const int idx = my * mb_width_ + mx;
        bool intra = intra_picture;
        Vec mv{};
        if (!intra_picture) {
          int sad = 0;
          const Vec left = mx > 0 ? mvs_[static_cast<std::size_t>(idx - 1)] : Vec{};
          const Vec top = my > 0 ? mvs_[static_cast<std::size_t>(idx - mb_width_)] : Vec{};
          mv = search(mx, my, {Vec{}, left, top, previous_mvs_.empty() ? Vec{} : previous_mvs_[static_cast<std::size_t>(idx)]}, sad);
          const int zero = luma_sad(mx, my, Vec{});
          if (zero <= sad + 32) {
            mv = Vec{};
            sad = zero;
          }
          if (intra_cost(mx, my) + 500 < sad) intra = true;
        }
        // Prediction and residual.
        std::array<std::uint8_t, 384> pred{};
        if (!intra) {
          mc_half(pred.data(), 16, view(reference_, 0), mx * 32 + mv.x, my * 32 + mv.y, 16, 16, 0, false);
          const int cx = chroma_from_half(mv.x);
          const int cy = chroma_from_half(mv.y);
          for (int p = 1; p < 3; ++p) {
            mc_half(pred.data() + 256 + (p - 1) * 64, 8, view(reference_, p), mx * 16 + cx, my * 16 + cy, 8, 8, 0, false);
          }
        }
        std::array<std::array<int, 64>, 6> levels{};
        std::array<std::array<std::int32_t, 64>, 6> recon{};
        int cbp = 0;
        std::array<int, 6> dc_diff{};
        for (int b = 0; b < 6; ++b) {
          const int p = b < 4 ? 0 : b - 3;
          const int bx = p == 0 ? mx * 16 + (b & 1) * 8 : mx * 8;
          const int by = p == 0 ? my * 16 + (b >> 1) * 8 : my * 8;
          const std::uint8_t* s = source_.at(p, bx, by);
          const std::uint8_t* pr = p == 0 ? pred.data() + (b >> 1) * 128 + (b & 1) * 8 : pred.data() + 256 + (p - 1) * 64;
          const int ps = p == 0 ? 16 : 8;
          std::int32_t samples[64];
          for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
              const int v = s[y * source_.stride[static_cast<std::size_t>(p)] + x];
              samples[y * 8 + x] = intra ? v : v - pr[y * ps + x];
            }
          }
          const int dc_scale = flavor_ == Flavor::Mpeg4 ? dc_scaler(q, p != 0) : 8;
          const bool ac = quantise(samples, intra, q, levels[static_cast<std::size_t>(b)], recon[static_cast<std::size_t>(b)], dc_scale);
          if (intra) {
            // AC presence decides the coded block pattern; the DC is always sent.
            if (ac) cbp |= 32 >> b;
            if (flavor_ == Flavor::Mpeg4) {
              // DC prediction exactly as the decoder does it.
              const int dc = recon[static_cast<std::size_t>(b)][0];
              const auto lookup = [&](int dx, int dy) -> int {
                if (b < 4) {
                  const int gx = mx * 2 + (b & 1) + dx;
                  const int gy = my * 2 + (b >> 1) + dy;
                  if (gx < 0 || gy < 0 || gx >= mb_width_ * 2) return 1024;
                  const int v = dc_luma[static_cast<std::size_t>(gy * mb_width_ * 2 + gx)];
                  return v < 0 ? 1024 : v;
                }
                const int nx = mx + dx;
                const int ny = my + dy;
                if (nx < 0 || ny < 0 || nx >= mb_width_) return 1024;
                const int v = dc_chroma[static_cast<std::size_t>((ny * mb_width_ + nx) * 2 + (b - 4))];
                return v < 0 ? 1024 : v;
              };
              const int fa = lookup(-1, 0);
              const int fb = lookup(-1, -1);
              const int fc = lookup(0, -1);
              const int pred_dc = std::abs(fa - fb) < std::abs(fb - fc) ? fc : fa;
              dc_diff[static_cast<std::size_t>(b)] = levels[static_cast<std::size_t>(b)][0] - rounded_div(pred_dc, dc_scale);
              if (b < 4) {
                dc_luma[static_cast<std::size_t>((my * 2 + (b >> 1)) * mb_width_ * 2 + mx * 2 + (b & 1))] = dc;
              } else {
                dc_chroma[static_cast<std::size_t>(idx * 2 + (b - 4))] = dc;
              }
            }
          } else if (ac) {
            cbp |= 32 >> b;
          }
        }
        if (!intra) {
          // Inter macroblocks leave no DC predictors.
          for (int b = 0; b < 4; ++b) dc_luma[static_cast<std::size_t>((my * 2 + (b >> 1)) * mb_width_ * 2 + mx * 2 + (b & 1))] = -1;
          dc_chroma[static_cast<std::size_t>(idx * 2)] = dc_chroma[static_cast<std::size_t>(idx * 2 + 1)] = -1;
        }

        // Header.
        const Vec pred_mv = predictor(mx, my);
        if (!intra_picture) {
          if (!intra && cbp == 0 && mv == Vec{}) {
            w.put(1, 1);  // not coded
            mvs_[static_cast<std::size_t>(idx)] = Vec{};
            reconstruct(pred, recon, 0, false, mx, my);
            continue;
          }
          w.put(1, 0);
        }
        const int cbpc = cbp & 3;
        const int cbpy = cbp >> 2;
        if (intra_picture) {
          const Code c = find(kMcbpcI, mcbpc(3, cbpc));
          w.put(c.length, c.bits);
        } else {
          const Code c = find(kMcbpcP, mcbpc(intra ? 3 : 0, cbpc));
          w.put(c.length, c.bits);
        }
        if (intra && flavor_ == Flavor::Mpeg4) w.put(1, 0);  // ac_pred_flag
        const Code cy = find(kCbpy, intra ? cbpy : 15 - cbpy);
        w.put(cy.length, cy.bits);
        if (!intra) {
          put_mvd(w, mv.x, pred_mv.x);
          put_mvd(w, mv.y, pred_mv.y);
        }
        mvs_[static_cast<std::size_t>(idx)] = intra ? Vec{} : mv;
        // Blocks.
        for (int b = 0; b < 6; ++b) {
          const auto& lv = levels[static_cast<std::size_t>(b)];
          if (intra) {
            if (flavor_ == Flavor::Mpeg4) {
              const int diff = dc_diff[static_cast<std::size_t>(b)];
              const int magnitude = std::abs(diff);
              int size = 0;
              while ((1 << size) <= magnitude) ++size;
              const Code c = find(b < 4 ? kDcSizeLuma : kDcSizeChroma, size);
              w.put(c.length, c.bits);
              if (size > 0) {
                w.put(size, static_cast<std::uint32_t>(diff > 0 ? diff : diff + (1 << size) - 1));
                if (size > 8) w.put(1, 1);
              }
            } else {
              const int dc = lv[0];
              w.put(8, static_cast<std::uint32_t>(dc == 128 ? 255 : dc));
            }
            if ((cbp & (32 >> b)) != 0) put_coefficients(w, lv, 1, flavor_ == Flavor::Mpeg4);
          } else if ((cbp & (32 >> b)) != 0) {
            put_coefficients(w, lv, 0, false);
          }
        }
        reconstruct(pred, recon, intra ? 0x3F : cbp, intra, mx, my);
      }
    }
    previous_mvs_ = mvs_;
  }

  void reconstruct(const std::array<std::uint8_t, 384>& pred, const std::array<std::array<std::int32_t, 64>, 6>& recon,
                   int coded, bool intra, int mx, int my) {
    for (int b = 0; b < 6; ++b) {
      const int p = b < 4 ? 0 : b - 3;
      const int bx = p == 0 ? mx * 16 + (b & 1) * 8 : mx * 8;
      const int by = p == 0 ? my * 16 + (b >> 1) * 8 : my * 8;
      std::uint8_t* d = current_.at(p, bx, by);
      const int stride = current_.stride[static_cast<std::size_t>(p)];
      if (intra) {
        // The DC is always present; AC only when coded.
        std::array<std::int32_t, 64> f = recon[static_cast<std::size_t>(b)];
        if ((coded & (32 >> b)) == 0) std::fill(f.begin() + 1, f.end(), 0);
        idct8x8_put(f.data(), d, stride);
        continue;
      }
      const std::uint8_t* pr = p == 0 ? pred.data() + (b >> 1) * 128 + (b & 1) * 8 : pred.data() + 256 + (p - 1) * 64;
      const int ps = p == 0 ? 16 : 8;
      for (int y = 0; y < 8; ++y) std::memcpy(d + y * stride, pr + y * ps, 8);
      if ((coded & (32 >> b)) != 0) idct8x8_add(recon[static_cast<std::size_t>(b)].data(), d, stride);
    }
  }

  Flavor flavor_;
  VideoEncodeParams params_;
  ColorInfo color_;
  int width_ = 0;
  int height_ = 0;
  int mb_width_ = 0;
  int mb_height_ = 0;
  int h263_format_ = 0;
  int gop_ = 250;
  int search_range_ = 15;
  int base_q_ = 3;
  double bitrate_ = 0;
  double rc_q_ = 8;
  double rc_debt_ = 0;
  double fps_ = 25;
  int time_resolution_ = 25;
  int time_increment_ = 1;
  int time_bits_ = 5;
  std::uint64_t last_second_ = 0;
  std::uint64_t frame_index_ = 0;

  TcoefCodes intra_codes_{kTcoefIntra};
  TcoefCodes inter_codes_{kTcoefInter};
  std::vector<std::uint8_t> headers_;
  Picture reference_;
  Picture current_;
  Picture source_;
  std::vector<Vec> mvs_;
  std::vector<Vec> previous_mvs_;
  std::deque<Packet> ready_;
};

}  // namespace

std::unique_ptr<codec::VideoEncoder> make_encoder(Flavor flavor) { return std::make_unique<Mpeg4Encoder>(flavor); }

}  // namespace cvt::legacy::mpeg4
