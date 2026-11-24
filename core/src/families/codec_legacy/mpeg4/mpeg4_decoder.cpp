// Convertible core - codec_legacy family: MPEG-4 Part 2 (ISO/IEC 14496-2),
// H.263 (ITU-T H.263) and Sorenson Spark video decoder.
//
// MPEG-4: rectangular video objects of the Simple and Advanced Simple
// profiles: I/P/B/S(GMC)-VOPs, H.263 and MPEG quantisation with custom
// matrices, AC/DC prediction, 4MV, quarter-sample motion compensation, global
// motion compensation with one warping point, interlaced field DCT and field
// motion vectors, resync markers / video packets, data partitioning (without
// reversible VLC: partitions coded with RVLC are concealed), packed bitstreams
// and not-coded VOPs, configuration in extradata or in band.
// H.263: baseline, PLUSPTYPE with custom picture sizes and rounding type,
// unrestricted motion vectors (Annex D, including the H.263+ extended MVD
// codes), GOB headers. Sorenson Spark: both escape versions, disposable frames.
//
// Output timestamps: every packet contributes its pts (or, without one, its
// dts - AVI convention); output frames take the smallest pending value.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "util/bytes.hpp"
#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/dct.hpp"
#include "families/codec_legacy/common/vlc.hpp"
#include "families/codec_legacy/mpeg4/mpeg4.hpp"
#include "families/codec_legacy/mpeg4/mpeg4_common.hpp"
#include "families/codec_legacy/mpeg4/mpeg4_tables.hpp"

namespace cvt::legacy::mpeg4 {

namespace {

constexpr int kI = 0;
constexpr int kP = 1;
constexpr int kB = 2;
constexpr int kS = 3;

// Macroblock kinds stored for prediction and B direct mode.
constexpr std::uint8_t kMbSkipped = 0;
constexpr std::uint8_t kMbInter = 1;
constexpr std::uint8_t kMbIntra = 2;
constexpr std::uint8_t kMbGmc = 3;

struct Tables {
  Vlc mcbpc_i{kMcbpcI};
  Vlc mcbpc_p{kMcbpcP};
  Vlc cbpy{kCbpy};
  Vlc mvd{kMvd};
  Vlc dc_luma{kDcSizeLuma};
  Vlc dc_chroma{kDcSizeChroma};
  Vlc tcoef_inter{kTcoefInter, 10};
  Vlc tcoef_intra{kTcoefIntra, 10};
  Vlc modb{kModb};
  Vlc b_type{kBMbType};
  Vlc dmv_length{kDmvLength};
  // Escape helpers: largest level per (last, run) and largest run per (last, level).
  std::array<std::array<std::uint8_t, 64>, 2> lmax_inter{};
  std::array<std::array<std::uint8_t, 64>, 2> lmax_intra{};
  std::array<std::array<std::uint8_t, 64>, 2> rmax_inter{};
  std::array<std::array<std::uint8_t, 64>, 2> rmax_intra{};

  Tables() {
    fill(kTcoefInter, lmax_inter, rmax_inter);
    fill(kTcoefIntra, lmax_intra, rmax_intra);
  }
  template <std::size_t N>
  static void fill(const VlcCode (&table)[N], std::array<std::array<std::uint8_t, 64>, 2>& lmax,
                   std::array<std::array<std::uint8_t, 64>, 2>& rmax) {
    for (const VlcCode& c : table) {
      if (c.symbol == kEscape) continue;
      const int last = c.symbol >> 12;
      const int run = (c.symbol >> 6) & 63;
      const int level = c.symbol & 63;
      auto& l = lmax[static_cast<std::size_t>(last)][static_cast<std::size_t>(run)];
      l = static_cast<std::uint8_t>(std::max<int>(l, level));
      auto& r = rmax[static_cast<std::size_t>(last)][static_cast<std::size_t>(level)];
      r = static_cast<std::uint8_t>(std::max<int>(r, run));
    }
  }
};

struct Vec {
  int x = 0;
  int y = 0;
};

struct MbInfo {
  std::uint8_t kind = kMbSkipped;
  bool not_coded = false;          // skipped in a P/S-VOP (B-VOPs then skip it too)
  bool four = false;               // four vectors (inter4v)
  bool field = false;              // field-predicted (interlaced)
  std::array<Vec, 4> mv{};         // per 8x8 block (half or quarter samples)
  std::array<Vec, 2> field_mv{};   // top / bottom field vectors
  std::array<std::uint8_t, 2> field_select{};
  int packet = -1;                 // video packet / GOB index
  int qp = 1;
};

/// Intra prediction data of one 8x8 block.
struct BlockPred {
  int dc = 1024;
  std::array<std::int16_t, 7> row{};  // first row AC (quantised), u = 1..7
  std::array<std::int16_t, 7> col{};  // first column AC (quantised), v = 1..7
  int qp = 1;
  bool intra = false;
};

struct PendingFrame {
  VideoFrame frame;
};

class Mpeg4Decoder final : public codec::VideoDecoder {
 public:
  explicit Mpeg4Decoder(Flavor flavor) : flavor_(flavor) {}

  Result<void> configure(const CodecParams& params) override {
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 1000;
    if (!params.extradata.empty() && flavor_ == Flavor::Mpeg4) {
      CVT_TRY(parse_units(as_u8(ByteView(params.extradata)), true));
    }
    return {};
  }

  Result<void> send_packet(const Packet& packet) override {
    if (packet.bytes.empty()) return {};
    const std::int64_t ts = packet.pts != INT64_MIN ? packet.pts : packet.dts;
    if (ts != INT64_MIN) timestamps_.insert(ticks_to_ms(ts, params_.timescale));
    if (packet.duration > 0) packet_duration_ms_ = ticks_to_ms(packet.duration, params_.timescale);
    const auto data = as_u8(ByteView(packet.bytes));
    if (flavor_ == Flavor::Flv1) return decode_flv_picture(data);
    if (flavor_ == Flavor::H263 || is_short_header(data)) return decode_h263_picture(data);
    return parse_units(data, false);
  }

  Result<std::optional<VideoFrame>> receive_frame() override {
    if (output_.empty()) return std::optional<VideoFrame>{};
    VideoFrame frame = std::move(output_.front());
    output_.pop_front();
    return std::optional<VideoFrame>(std::move(frame));
  }

  Result<void> flush() override {
    if (held_) {
      emit(std::move(*held_));
      held_.reset();
    }
    return {};
  }

 private:
  // ---- framing ----------------------------------------------------------------------

  static bool is_short_header(std::span<const std::uint8_t> d) {
    return d.size() >= 3 && d[0] == 0 && d[1] == 0 && (d[2] & 0xFC) == 0x80;
  }

  /// Splits MPEG-4 data at start codes and handles each unit.
  Result<void> parse_units(std::span<const std::uint8_t> data, bool headers_only) {
    std::size_t at = 0;
    const auto next_code = [&](std::size_t from) {
      for (std::size_t i = from; i + 3 < data.size(); ++i) {
        if (data[i + 2] > 1) {
          i += 2;
          continue;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
      }
      return data.size();
    };
    at = next_code(0);
    while (at < data.size()) {
      const std::uint8_t code = data[at + 3];
      const std::size_t end = code == 0xB6 ? data.size() : next_code(at + 4);
      // A VOP runs to the next VOP/VOL/VOS/GOV code (resync markers are not start codes).
      std::size_t stop = end;
      if (code == 0xB6) {
        stop = at + 4;
        while (true) {
          const std::size_t n = next_code(stop);
          if (n >= data.size()) {
            stop = data.size();
            break;
          }
          const std::uint8_t c = data[n + 3];
          if (c == 0xB6 || c == 0xB0 || c == 0xB3 || c == 0xB5 || c <= 0x2F) {
            stop = n;
            break;
          }
          stop = n + 4;
        }
      }
      const auto unit = data.subspan(at + 4, stop - at - 4);
      if (code >= 0x20 && code <= 0x2F) {
        CVT_TRY(parse_vol(unit));
      } else if (code == 0xB5) {
        parse_visual_object(unit);
      } else if (code == 0xB2) {
        parse_user_data(unit);
      } else if (code == 0xB6 && !headers_only) {
        CVT_TRY(decode_vop(unit));
      }
      at = stop < data.size() ? next_code(stop) : data.size();
    }
    return {};
  }

  void parse_user_data(std::span<const std::uint8_t> unit) {
    // DivX writes "DivX503b1393p": the trailing 'p' marks a packed bitstream.
    const std::string_view text(reinterpret_cast<const char*>(unit.data()), unit.size());
    if (text.starts_with("DivX") && text.find('p') != std::string_view::npos) packed_ = true;
  }

  void parse_visual_object(std::span<const std::uint8_t> unit) {
    BitReader br(unit);
    if (br.bit()) br.skip(4 + 3);  // verid, priority
    const std::uint32_t type = br.bits(4);
    if ((type == 1 || type == 2) && br.bit()) {  // video_signal_type
      br.skip(3);                                // video_format
      full_range_ = br.bit();
      if (br.bit()) {                            // colour_description
        br.skip(16);
        matrix_ = static_cast<int>(br.bits(8));
      }
    }
  }

  static void read_matrix(BitReader& br, std::array<std::uint8_t, 64>& m) {
    int last = 8;
    for (int i = 0; i < 64; ++i) {
      const int v = static_cast<int>(br.bits(8));
      if (v == 0) {
        for (int k = i; k < 64; ++k) m[zigzag[k]] = static_cast<std::uint8_t>(last);
        return;
      }
      last = v;
      m[zigzag[i]] = static_cast<std::uint8_t>(v);
    }
  }

  Result<void> parse_vol(std::span<const std::uint8_t> unit) {
    BitReader br(unit);
    br.skip(1);  // random_accessible_vol
    br.skip(8);  // video_object_type_indication
    int verid = 1;
    if (br.bit()) {
      verid = static_cast<int>(br.bits(4));
      br.skip(3);
    }
    if (br.bits(4) == 15) br.skip(16);  // aspect ratio (extended PAR)
    if (br.bit()) {                     // vol_control_parameters
      br.skip(2);                       // chroma_format
      low_delay_ = br.bit();
      if (br.bit()) br.skip(15 + 1 + 15 + 1 + 15 + 1 + 3 + 11 + 1 + 15 + 1);  // vbv_parameters
    }
    const std::uint32_t shape = br.bits(2);
    if (shape != 0) return fail(Status::Decode, "MPEG-4 video with object shapes is not supported");
    br.skip(1);
    time_resolution_ = static_cast<int>(br.bits(16));
    br.skip(1);
    if (time_resolution_ == 0) return fail(Status::Decode, "MPEG-4 video has a zero time resolution");
    time_bits_ = 1;
    while ((1 << time_bits_) < time_resolution_) ++time_bits_;
    fixed_increment_ = 0;
    if (br.bit()) fixed_increment_ = static_cast<int>(br.bits(time_bits_));
    br.skip(1);
    const std::uint32_t w = br.bits(13);
    br.skip(1);
    const std::uint32_t h = br.bits(13);
    br.skip(1);
    interlaced_ = br.bit();
    br.skip(1);  // obmc_disable
    sprite_ = static_cast<int>(br.bits(verid == 1 ? 1 : 2));
    warp_points_ = 0;
    if (sprite_ == 1 || sprite_ == 2) {
      if (sprite_ != 2) br.skip(13 + 1 + 13 + 1 + 13 + 1 + 13 + 1);
      warp_points_ = static_cast<int>(br.bits(6));
      warp_accuracy_ = static_cast<int>(br.bits(2));
      br.skip(1);  // sprite_brightness_change
      if (sprite_ != 2) br.skip(1);
    }
    if (sprite_ == 1) return fail(Status::Decode, "MPEG-4 static sprites are not supported");
    if (br.bit()) return fail(Status::Decode, "MPEG-4 video with more than 8 bits per sample is not supported");
    mpeg_quant_ = br.bit();
    std::copy(std::begin(kDefaultIntra), std::end(kDefaultIntra), intra_matrix_.begin());
    std::copy(std::begin(kDefaultInter), std::end(kDefaultInter), inter_matrix_.begin());
    if (mpeg_quant_) {
      if (br.bit()) read_matrix(br, intra_matrix_);
      if (br.bit()) read_matrix(br, inter_matrix_);
    }
    quarter_sample_ = verid != 1 && br.bit();
    if (!br.bit()) {  // complexity_estimation_disable == 0
      return fail(Status::Decode, "MPEG-4 complexity estimation headers are not supported");
    }
    resync_disabled_ = br.bit();
    data_partitioned_ = br.bit();
    rvlc_ = data_partitioned_ && br.bit();
    if (verid != 1) {
      if (br.bit()) return fail(Status::Decode, "MPEG-4 NEWPRED is not supported");
      if (br.bit()) return fail(Status::Decode, "MPEG-4 reduced resolution VOPs are not supported");
    }
    if (br.bit()) return fail(Status::Decode, "scalable MPEG-4 video is not supported");
    if (br.overrun()) return fail(Status::Decode, "truncated MPEG-4 video object layer header");
    CVT_TRY(set_size(w, h));
    vol_seen_ = true;
    return {};
  }

  Result<void> set_size(std::uint32_t w, std::uint32_t h) {
    CVT_TRY(check_dimensions(w, h));
    if (w == width_ && h == height_ && !pictures_[0].plane[0].empty()) return {};
    if (held_) {
      emit(std::move(*held_));
      held_.reset();
    }
    width_ = w;
    height_ = h;
    mb_width_ = static_cast<int>((w + 15) / 16);
    mb_height_ = static_cast<int>((h + 15) / 16);
    for (Picture& p : pictures_) p.allocate(mb_width_ * 16, mb_height_ * 16);
    const auto mbs = static_cast<std::size_t>(mb_width_) * static_cast<std::size_t>(mb_height_);
    mb_.assign(mbs, MbInfo{});
    ref_mb_.assign(mbs, MbInfo{});
    luma_pred_.assign(mbs * 4, BlockPred{});
    chroma_pred_.assign(mbs * 2, BlockPred{});
    forward_ = backward_ = -1;
    return {};
  }

  // ---- VOP / picture headers -------------------------------------------------------------

  Result<void> decode_vop(std::span<const std::uint8_t> unit) {
    if (!vol_seen_) return {};  // no configuration yet: wait for a VOL
    BitReader br(unit);
    type_ = static_cast<int>(br.bits(2));
    int modulo = 0;
    while (br.bit()) {
      if (++modulo > 60 || br.overrun()) return {};
    }
    br.skip(1);
    const int increment = static_cast<int>(br.bits(time_bits_));
    br.skip(1);
    if (!br.bit()) return {};  // not coded: packed-bitstream placeholder or dropped frame
    // VOP times (direct mode needs TRB/TRD).
    if (type_ != kB) {
      last_time_base_ = time_base_;
      time_base_ += modulo;
      const std::int64_t t = time_base_ * time_resolution_ + increment;
      pp_time_ = static_cast<int>(t - last_non_b_time_);
      last_non_b_time_ = t;
    } else {
      const std::int64_t t = (last_time_base_ + modulo) * time_resolution_ + increment;
      pb_time_ = static_cast<int>(pp_time_ - (last_non_b_time_ - t));
    }
    rounding_ = (type_ == kP || (type_ == kS && sprite_ == 2)) ? (br.bit() ? 1 : 0) : 0;
    intra_dc_threshold_ = kDcThreshold[br.bits(3)];
    alternate_vertical_scan_ = false;
    if (interlaced_) {
      br.skip(1);  // top_field_first
      alternate_vertical_scan_ = br.bit();
    }
    if (type_ == kS) {
      if (sprite_ != 2) return {};
      CVT_TRY(sprite_trajectory(br));
    }
    qp_ = static_cast<int>(br.bits(5));
    fcode_forward_ = fcode_backward_ = 1;
    if (type_ != kI) fcode_forward_ = static_cast<int>(br.bits(3));
    if (type_ == kB) fcode_backward_ = static_cast<int>(br.bits(3));
    if (br.overrun() || qp_ == 0 || fcode_forward_ == 0 || fcode_backward_ == 0) return {};
    short_header_ = false;
    return decode_picture_data(br);
  }

  Result<void> sprite_trajectory(BitReader& br) {
    std::array<Vec, 4> d{};
    for (int i = 0; i < warp_points_ && i < 4; ++i) {
      for (int t = 0; t < 2; ++t) {
        const int length = tables_.dmv_length.decode(br);
        if (length == Vlc::invalid) return fail(Status::Decode, "damaged MPEG-4 sprite trajectory");
        int v = 0;
        if (length > 0) {
          v = static_cast<int>(br.bits(length));
          if ((v >> (length - 1)) == 0) v = v - (1 << length) + 1;
        }
        br.skip(1);
        (t == 0 ? d[static_cast<std::size_t>(i)].x : d[static_cast<std::size_t>(i)].y) = v;
      }
    }
    if (warp_points_ > 1) return fail(Status::Decode, "MPEG-4 GMC with {} warping points is not supported", warp_points_);
    // One warping point: a translation in 1/s sample units, s = 2 << accuracy.
    const int s = 2 << warp_accuracy_;
    gmc_offset_ = {d[0].x * (s / 2), d[0].y * (s / 2)};
    return {};
  }

  Result<void> decode_h263_picture(std::span<const std::uint8_t> data) {
    BitReader br(data);
    if (br.bits(22) != 0x20) return {};  // no picture start code: skip
    br.skip(8);                          // temporal reference
    if (!br.bit() || br.bit()) return {};  // PTYPE marker bits
    br.skip(3);                          // split screen, document camera, freeze picture release
    const int format = static_cast<int>(br.bits(3));
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    umv_ = false;
    rounding_ = 0;
    if (format != 7) {
      static constexpr std::uint32_t kW[6] = {0, 128, 176, 352, 704, 1408};
      static constexpr std::uint32_t kH[6] = {0, 96, 144, 288, 576, 1152};
      if (format == 0 || format > 5) return fail(Status::Decode, "unsupported H.263 source format {}", format);
      w = kW[format];
      h = kH[format];
      type_ = br.bit() ? kP : kI;
      umv_ = br.bit();
      if (br.bit()) return fail(Status::Decode, "H.263 syntax-based arithmetic coding is not supported");
      if (br.bit()) return fail(Status::Decode, "H.263 advanced prediction mode is not supported");
      if (br.bit()) return fail(Status::Decode, "H.263 PB-frames are not supported");
      qp_ = static_cast<int>(br.bits(5));
      if (br.bit()) br.skip(2);  // CPM + PSBI
    } else {
      const std::uint32_t ufep = br.bits(3);
      int source = static_cast<int>(width_ != 0 ? last_h263_format_ : 0);
      if (ufep == 1) {
        source = static_cast<int>(br.bits(3));
        custom_pcf_ = br.bit();
        plus_umv_ = br.bit();
        if (br.bit()) return fail(Status::Decode, "H.263 syntax-based arithmetic coding is not supported");
        if (br.bit()) return fail(Status::Decode, "H.263 advanced prediction (Annex F) is not supported");
        if (br.bit()) return fail(Status::Decode, "H.263 advanced intra coding (Annex I) is not supported");
        if (br.bit()) return fail(Status::Decode, "H.263 deblocking filter (Annex J) is not supported");
        slice_structured_ = br.bit();  // only single-slice pictures are decoded
        if (br.bit()) return fail(Status::Decode, "H.263 reference picture selection is not supported");
        br.skip(1);  // independent segment decoding
        if (br.bit()) return fail(Status::Decode, "H.263 alternative inter VLC (Annex S) is not supported");
        if (br.bit()) return fail(Status::Decode, "H.263 modified quantisation (Annex T) is not supported");
        br.skip(4);  // marker + reserved
        last_h263_format_ = source;
      } else if (ufep != 0) {
        return {};
      }
      const int ptype = static_cast<int>(br.bits(3));
      if (ptype > 1) return fail(Status::Decode, "H.263 picture type {} is not supported", ptype);
      type_ = ptype == 0 ? kI : kP;
      if (br.bit()) return fail(Status::Decode, "H.263 reference picture resampling is not supported");
      if (br.bit()) return fail(Status::Decode, "H.263 reduced resolution update is not supported");
      rounding_ = br.bit() ? 1 : 0;
      br.skip(3);  // reserved + marker
      if (br.bit()) br.skip(2);  // CPM + PSBI
      if (source == 6) {
        if (ufep == 1) {
          const std::uint32_t par = br.bits(4);
          custom_w_ = (br.bits(9) + 1) * 4;
          br.skip(1);
          custom_h_ = br.bits(9) * 4;
          if (par == 15) br.skip(16);
        }
        w = custom_w_;
        h = custom_h_;
      }
      if (custom_pcf_) {
        if (ufep == 1) br.skip(8);  // CPCFC
        br.skip(2);                 // ETR
      }
      if (source != 6) {
        static constexpr std::uint32_t kW[6] = {0, 128, 176, 352, 704, 1408};
        static constexpr std::uint32_t kH[6] = {0, 96, 144, 288, 576, 1152};
        if (source < 1 || source > 5) return fail(Status::Decode, "unsupported H.263 source format {}", source);
        w = kW[source];
        h = kH[source];
      }
      umv_ = plus_umv_;
      if (umv_ && ufep == 1 && !br.bit()) br.skip(1);  // UUI: "1" or "01"
      if (slice_structured_ && ufep == 1) br.skip(2);   // SSS
      qp_ = static_cast<int>(br.bits(5));
    }
    while (br.bit()) br.skip(8);  // PEI / PSPARE
    if (br.overrun() || qp_ == 0) return {};
    CVT_TRY(set_size(w, h));
    if (format == 7 && slice_structured_) {
      // Annex K: the first slice header follows (SEPB1, MBA, SEPB2).
      static constexpr int kMbaMax[6] = {47, 98, 395, 1583, 6335, 9215};
      static constexpr int kMbaBits[6] = {6, 7, 9, 11, 13, 14};
      int k = 0;
      while (k < 5 && mb_width_ * mb_height_ - 1 > kMbaMax[k]) ++k;
      br.skip(1);
      if (br.bits(kMbaBits[k]) != 0) return {};
      br.skip(1);
    }
    short_header_ = true;
    flv_version_ = -1;
    fcode_forward_ = 1;
    intra_dc_threshold_ = 99;
    alternate_vertical_scan_ = false;
    mpeg_quant_ = false;
    quarter_sample_ = false;
    data_partitioned_ = false;
    resync_disabled_ = true;
    return decode_picture_data(br);
  }

  Result<void> decode_flv_picture(std::span<const std::uint8_t> data) {
    BitReader br(data);
    if (br.bits(17) != 1) return {};
    const int version = static_cast<int>(br.bits(5));
    if (version > 1) return fail(Status::Decode, "unsupported Sorenson Spark version {}", version);
    br.skip(8);  // temporal reference
    const std::uint32_t format = br.bits(3);
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    switch (format) {
      case 0: w = br.bits(8); h = br.bits(8); break;
      case 1: w = br.bits(16); h = br.bits(16); break;
      case 2: w = 352; h = 288; break;
      case 3: w = 176; h = 144; break;
      case 4: w = 128; h = 96; break;
      case 5: w = 320; h = 240; break;
      case 6: w = 160; h = 120; break;
      default: return fail(Status::Decode, "reserved Sorenson Spark picture size");
    }
    const std::uint32_t ptype = br.bits(2);
    if (ptype == 3) return {};
    type_ = ptype == 0 ? kI : kP;
    br.skip(1);  // deblocking flag (a post-filter hint)
    qp_ = static_cast<int>(br.bits(5));
    while (br.bit()) br.skip(8);
    if (br.overrun() || qp_ == 0) return {};
    CVT_TRY(set_size(w, h));
    disposable_ = ptype == 2;
    short_header_ = true;
    flv_version_ = version;
    umv_ = true;  // Sorenson vectors may point outside the picture
    rounding_ = 0;
    fcode_forward_ = 1;
    intra_dc_threshold_ = 99;
    alternate_vertical_scan_ = false;
    mpeg_quant_ = false;
    quarter_sample_ = false;
    data_partitioned_ = false;
    resync_disabled_ = true;
    return decode_picture_data(br);
  }

  // ---- picture data --------------------------------------------------------------------

  Result<void> decode_picture_data(BitReader& br) {
    if (type_ == kB) {
      if (forward_ < 0 || backward_ < 0) return {};  // no references (open GOV at the start)
      current_ = 0;
      while (current_ == forward_ || current_ == backward_) ++current_;
    } else {
      if (backward_ >= 0) forward_ = backward_;
      current_ = 0;
      while (current_ == forward_ || current_ == backward_) ++current_;
      if (type_ != kI && forward_ < 0) {
        // P picture without a reference (damaged start): predict from grey.
        forward_ = current_ == 0 ? 1 : 0;
        for (std::size_t p = 0; p < 3; ++p) {
          auto& plane = pictures_[static_cast<std::size_t>(forward_)].plane[p];
          std::fill(plane.begin(), plane.end(), p == 0 ? 16 : 128);
        }
      }
    }
    for (MbInfo& m : mb_) m = MbInfo{};
    for (BlockPred& b : luma_pred_) b = BlockPred{};
    for (BlockPred& b : chroma_pred_) b = BlockPred{};
    decoded_.assign(mb_.size(), 0);
    packet_ = 0;

    if (data_partitioned_ && !short_header_) {
      decode_partitioned(br);
    } else {
      decode_combined(br);
    }
    conceal();

    // Output and reference management.
    VideoFrame frame = make_frame(pictures_[static_cast<std::size_t>(current_)]);
    if (type_ == kB) {
      emit(std::move(frame));
    } else {
      if (!(flavor_ == Flavor::Flv1 && disposable_)) {
        backward_ = current_;
        ref_mb_ = mb_;
      }
      if (held_) emit(std::move(*held_));
      held_ = std::move(frame);
      if (short_header_ || low_delay_) {
        emit(std::move(*held_));
        held_.reset();
      }
    }
    return {};
  }

  /// Fills macroblocks that could not be decoded from the forward reference.
  void conceal() {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    const int ref = type_ == kI ? -1 : forward_;
    for (int my = 0; my < mb_height_; ++my) {
      for (int mx = 0; mx < mb_width_; ++mx) {
        if (decoded_[static_cast<std::size_t>(my * mb_width_ + mx)] != 0) continue;
        for (int p = 0; p < 3; ++p) {
          const int n = p == 0 ? 16 : 8;
          for (int y = 0; y < n; ++y) {
            std::uint8_t* d = cur.at(p, mx * n, my * n + y);
            if (ref >= 0) {
              ::cvt::util::copy_bytes(d, pictures_[static_cast<std::size_t>(ref)].at(p, mx * n, my * n + y), static_cast<std::size_t>(n));
            } else {
              ::cvt::util::fill_bytes(d, p == 0 ? 16 : 128, static_cast<std::size_t>(n));
            }
          }
        }
      }
    }
  }

  [[nodiscard]] VideoFrame make_frame(const Picture& pic) const {
    VideoFrame frame = VideoFrame::allocate(PixelFormat::I420, width_, height_);
    for (std::size_t p = 0; p < 3; ++p) {
      const std::size_t row = ir::plane_row_bytes(PixelFormat::I420, p, width_);
      const std::size_t rows = ir::plane_height(PixelFormat::I420, p, height_);
      for (std::size_t y = 0; y < rows; ++y) {
        ::cvt::util::copy_bytes(frame.planes[p].data.data() + y * frame.planes[p].stride,
                    pic.plane[p].data() + y * static_cast<std::size_t>(pic.stride[p]), row);
      }
    }
    frame.color.range = full_range_ ? ir::ColorRange::Full : ir::ColorRange::Limited;
    frame.color.matrix = matrix_ == 1 ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601;
    frame.color.siting = short_header_ ? ir::ChromaSiting::Center : ir::ChromaSiting::Left;
    std::int64_t duration = packet_duration_ms_;
    if (fixed_increment_ > 0 && time_resolution_ > 0 && !short_header_) {
      duration = std::max<std::int64_t>(1, 1000LL * fixed_increment_ / time_resolution_);
    }
    frame.duration_ms = duration > 0 ? duration : 40;
    return frame;
  }

  void emit(VideoFrame frame) {
    if (!timestamps_.empty()) {
      frame.pts_ms = *timestamps_.begin();
      timestamps_.erase(timestamps_.begin());
    } else {
      frame.pts_ms = have_last_ ? last_pts_ + last_duration_ : 0;
    }
    if (have_last_ && frame.pts_ms <= last_pts_) frame.pts_ms = last_pts_ + 1;
    have_last_ = true;
    last_pts_ = frame.pts_ms;
    last_duration_ = frame.duration_ms;
    output_.push_back(std::move(frame));
  }

  // ---- macroblock loop -------------------------------------------------------------------

  /// Length of the zero run of a resync marker (a '1' follows): 16 in I-VOPs,
  /// 15 + fcode in P/S-VOPs, 15 + max(fcodes, 2) in B-VOPs. Decoders seen in
  /// the wild disagree on the B-VOP length by one, so 16..22 zeros are accepted.
  [[nodiscard]] static int marker_zeros(BitReader probe) {
    int zeros = 0;
    while (zeros < 23 && probe.peek(1) == 0) {
      if (probe.bits_left() == 0) return 0;
      probe.skip(1);
      ++zeros;
    }
    return (zeros >= 16 && zeros <= 22 && probe.peek(1) == 1) ? zeros : 0;
  }

  /// True when a video packet header (after stuffing) follows.
  [[nodiscard]] bool at_resync(BitReader& br) const {
    if (resync_disabled_) return false;
    const int stuffing = static_cast<int>(8 - br.position() % 8);  // 1..8
    const std::uint32_t pattern = (1u << (stuffing - 1)) - 1u;     // 0 then ones
    if (br.peek(stuffing) != pattern) return false;
    BitReader probe = br;
    probe.skip(stuffing);
    return marker_zeros(probe) != 0;
  }

  /// Parses a video packet header (positioned at its stuffing, or exactly at
  /// the marker when `aligned`); returns the macroblock number.
  int video_packet_header(BitReader& br, bool aligned = false) {
    if (!aligned) br.skip(static_cast<int>(8 - br.position() % 8));
    br.skip(marker_zeros(br) + 1);
    int bits = 1;
    const int mbs = mb_width_ * mb_height_;
    while ((1 << bits) < mbs) ++bits;
    const int mb_number = static_cast<int>(br.bits(bits));
    qp_ = std::max(1, static_cast<int>(br.bits(5)));
    if (br.bit()) {  // header_extension_code
      while (br.bit()) {
        if (br.overrun()) break;
      }
      br.skip(1 + time_bits_ + 1 + 2 + 3);
      if (type_ != kI) br.skip(3);
      if (type_ == kB) br.skip(3);
    }
    ++packet_;
    return mb_number;
  }

  /// H.263 Annex K slice header; returns the first macroblock of the slice, or -1.
  int h263_slice_header(BitReader& br) {
    BitReader probe = br;
    if (probe.peek(17) != 1) {
      probe.align();
      if (probe.peek(17) != 1) return -1;
    }
    probe.skip(17);
    probe.skip(1);  // SEPB1
    static constexpr int kMbaMax[6] = {47, 98, 395, 1583, 6335, 9215};
    static constexpr int kMbaBits[6] = {6, 7, 9, 11, 13, 14};
    int k = 0;
    const int mbs = mb_width_ * mb_height_;
    while (k < 5 && mbs - 1 > kMbaMax[k]) ++k;
    const int mba = static_cast<int>(probe.bits(kMbaBits[k]));
    if (mbs > 1583) probe.skip(1);  // SEPB2
    qp_ = std::max(1, static_cast<int>(probe.bits(5)));
    probe.skip(1 + 2);  // SEPB3, GFID
    if (probe.overrun()) return -1;
    br = probe;
    ++packet_;
    return mba;
  }

  /// H.263 GOB header at the start of a macroblock row (after optional stuffing).
  bool h263_gob_header(BitReader& br, int row) {
    BitReader probe = br;
    // GBSC may be preceded by stuffing to byte alignment.
    probe.align();
    if (probe.peek(17) != 1) {
      if (br.peek(17) != 1) return false;
      probe = br;
    }
    probe.skip(17);
    const int gn = static_cast<int>(probe.bits(5));
    if (gn != row) return false;
    probe.skip(2);  // GFID (no CPM)
    qp_ = std::max(1, static_cast<int>(probe.bits(5)));
    br = probe;
    ++packet_;
    return true;
  }

  void decode_combined(BitReader& br) {
    int mb = 0;
    const int count = mb_width_ * mb_height_;
    pmv_fwd_ = pmv_bwd_ = Vec{};
    while (mb < count) {
      const int mx = mb % mb_width_;
      const int my = mb / mb_width_;
      if (!short_header_ && at_resync(br)) {
        const int number = video_packet_header(br);
        if (number < mb || number >= count) return;
        mb = number;
        continue;
      }
      if (short_header_ && mx == 0 && my > 0 && flv_version_ < 0 && !slice_structured_) h263_gob_header(br, my);
      if (short_header_ && slice_structured_ && mb > 0 && br.peek(16) == 0) {
        const int start = h263_slice_header(br);
        if (start > mb && start < count) {
          mb = start;
          continue;
        }
        if (start >= 0 && start != mb) return;
      }
      if (mx == 0) pmv_fwd_ = pmv_bwd_ = Vec{};
      bool ok;
      if (type_ == kB) ok = decode_b_mb(br, mx, my);
      else ok = decode_ip_mb(br, mx, my);
      if (!ok || br.overrun()) {
        // Damaged: skip to the next video packet (MPEG-4) or give up.
        if (short_header_ || resync_disabled_) return;
        if (!seek_resync(br)) return;
        const int number = video_packet_header(br, true);
        if (number <= mb || number >= count) return;
        mb = number;
        continue;
      }
      decoded_[static_cast<std::size_t>(mb)] = 1;
      ++mb;
    }
  }

  /// Scans forward to the next resync marker (always byte aligned) and
  /// stops exactly at it.
  bool seek_resync(BitReader& br) const {
    br.align();
    while (br.bits_left() > 24) {
      if (br.peek(16) == 0 && marker_zeros(br) != 0) return true;
      br.skip(8);
    }
    return false;
  }

  /// After a packet: moves to the next video packet header and parses it.
  /// Returns the next macroblock number, or -1 at the end of the data.
  int next_packet(BitReader& br) {
    if (at_resync(br)) return video_packet_header(br);
    if (!seek_resync(br)) return -1;
    return video_packet_header(br, true);
  }

  // ---- motion vectors ----------------------------------------------------------------------

  [[nodiscard]] bool available(int mx, int my) const {
    if (mx < 0 || my < 0 || mx >= mb_width_ || my >= mb_height_) return false;
    return mb_[static_cast<std::size_t>(my * mb_width_ + mx)].packet == packet_;
  }

  [[nodiscard]] Vec block_mv(int mx, int my, int block) const {
    const MbInfo& m = mb_[static_cast<std::size_t>(my * mb_width_ + mx)];
    if (m.kind == kMbIntra) return {};
    if (m.field) {
      // Field-predicted neighbour: the average of its field vectors (frame units).
      const int x = m.field_mv[0].x + m.field_mv[1].x;
      const int y = m.field_mv[0].y + m.field_mv[1].y;
      return {(x >> 1) | (x & 1), (y >> 1) | (y & 1)};
    }
    return m.mv[static_cast<std::size_t>(block)];
  }

  /// Median prediction for block `block` (0..3) of macroblock (mx, my).
  [[nodiscard]] Vec predict_mv(int mx, int my, int block, const std::array<Vec, 4>& current) const {
    Vec c[3];
    bool ok[3];
    // Left, above, above-right candidates.
    switch (block) {
      case 0:
        ok[0] = available(mx - 1, my); c[0] = ok[0] ? block_mv(mx - 1, my, 1) : Vec{};
        ok[1] = available(mx, my - 1); c[1] = ok[1] ? block_mv(mx, my - 1, 2) : Vec{};
        ok[2] = available(mx + 1, my - 1); c[2] = ok[2] ? block_mv(mx + 1, my - 1, 2) : Vec{};
        break;
      case 1:
        ok[0] = true; c[0] = current[0];
        ok[1] = available(mx, my - 1); c[1] = ok[1] ? block_mv(mx, my - 1, 3) : Vec{};
        ok[2] = available(mx + 1, my - 1); c[2] = ok[2] ? block_mv(mx + 1, my - 1, 2) : Vec{};
        break;
      case 2:
        ok[0] = available(mx - 1, my); c[0] = ok[0] ? block_mv(mx - 1, my, 3) : Vec{};
        ok[1] = true; c[1] = current[0];
        ok[2] = true; c[2] = current[1];
        break;
      default:
        ok[0] = true; c[0] = current[2];
        ok[1] = true; c[1] = current[0];
        ok[2] = true; c[2] = current[1];
        break;
    }
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

  bool decode_mv_component(BitReader& br, int fcode, int pred, int& out) {
    if (short_header_ && umv_ && flavor_ != Flavor::Flv1 && plus_umv_) {
      // H.263 Annex D with PLUSPTYPE: unrestricted extended MVD codes.
      if (br.bit()) {
        out = pred;
        return true;
      }
      int code = 2 + (br.bit() ? 1 : 0);
      while (br.bit()) {
        code = (code << 1) | (br.bit() ? 1 : 0);
        if (code >= 32768 || br.overrun()) return false;
      }
      const bool negative = (code & 1) != 0;
      code >>= 1;
      out = negative ? pred - code : pred + code;
      return true;
    }
    int code = tables_.mvd.decode(br);
    if (code == Vlc::invalid) return false;
    if (code != 0 && br.bit()) code = -code;
    const int shift = fcode - 1;
    int diff = code;
    if (shift > 0 && code != 0) {
      const int residual = static_cast<int>(br.bits(shift));
      diff = ((std::abs(code) - 1) << shift) + residual + 1;
      if (code < 0) diff = -diff;
    }
    int v = pred + diff;
    if (short_header_ && umv_ && flavor_ != Flavor::Flv1) {
      // H.263 Annex D (plain PTYPE): vectors may extend to [-31.5, 31.5].
      if (pred < -31 && v < -63) v += 64;
      if (pred > 32 && v > 63) v -= 64;
      out = v;
      return true;
    }
    // Wrap into [-32 f, 32 f - 1].
    const int bits = 5 + fcode;
    const int range = 1 << bits;
    v &= range - 1;
    if (v >= range / 2) v -= range;
    out = v;
    return true;
  }

  bool decode_mv(BitReader& br, int fcode, Vec pred, Vec& out) {
    return decode_mv_component(br, fcode, pred.x, out.x) && decode_mv_component(br, fcode, pred.y, out.y);
  }

  // ---- motion compensation ------------------------------------------------------------------

  /// Luma block prediction (16x16 or 8x8) at block position (bx, by) with vector v.
  void mc_luma(std::uint8_t* dst, int stride, const PlaneView& ref, int bx, int by, int n, Vec v, int r, bool avg) const {
    if (quarter_sample_) {
      mc_qpel(dst, stride, ref, bx * 4 + v.x, by * 4 + v.y, n, r, avg);
    } else {
      mc_half(dst, stride, ref, bx * 2 + v.x, by * 2 + v.y, n, n, r, avg);
    }
  }

  /// Chroma vector (half-sample chroma units) for a luma vector.
  [[nodiscard]] int chroma_component(int v) const {
    if (quarter_sample_) v /= 2;
    return chroma_from_half(v);
  }

  /// Predicts macroblock (mx, my) of the current picture from `ref` with
  /// one vector or four (`four`), averaging when `avg`.
  void predict_frame_mb(int ref, int mx, int my, const std::array<Vec, 4>& mv, bool four, int r, bool avg) {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    const Picture& pic = pictures_[static_cast<std::size_t>(ref)];
    const PlaneView luma = view(pic, 0);
    if (!four) {
      mc_luma(cur.at(0, mx * 16, my * 16), cur.stride[0], luma, mx * 16, my * 16, 16, mv[0], r, avg);
    } else {
      for (int b = 0; b < 4; ++b) {
        const int bx = mx * 16 + (b & 1) * 8;
        const int by = my * 16 + (b >> 1) * 8;
        mc_luma(cur.at(0, bx, by), cur.stride[0], luma, bx, by, 8, mv[static_cast<std::size_t>(b)], r, avg);
      }
    }
    Vec c;
    if (!four) {
      c = {chroma_component(mv[0].x), chroma_component(mv[0].y)};
    } else {
      int sx = 0;
      int sy = 0;
      for (const Vec& v : mv) {
        sx += quarter_sample_ ? v.x / 2 : v.x;
        sy += quarter_sample_ ? v.y / 2 : v.y;
      }
      c = {chroma_from_sum4(sx), chroma_from_sum4(sy)};
    }
    for (int p = 1; p < 3; ++p) {
      mc_half(cur.at(p, mx * 8, my * 8), cur.stride[static_cast<std::size_t>(p)], view(pic, p), mx * 16 + c.x,
              my * 16 + c.y, 8, 8, r, avg);
    }
  }

  /// Field prediction of macroblock (mx, my): field f of the current MB from
  /// field select[f] of `ref` with vector mv[f] (field units).
  void predict_field_mb(int ref, int mx, int my, const std::array<Vec, 2>& mv, const std::array<std::uint8_t, 2>& select,
                        int r, bool avg) {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    const Picture& pic = pictures_[static_cast<std::size_t>(ref)];
    for (int f = 0; f < 2; ++f) {
      const Vec v = mv[static_cast<std::size_t>(f)];
      const int sel = select[static_cast<std::size_t>(f)];
      // Luma 16x8 in field coordinates.
      PlaneView luma = view(pic, 0, sel);
      std::uint8_t* d = cur.at(0, mx * 16, my * 16 + f);
      if (quarter_sample_) {
        // Two 8x8 quarter-sample blocks per field half (the filter works on square blocks).
        for (int b = 0; b < 2; ++b) mc_qpel(d + b * 8, cur.stride[0] * 2, luma, (mx * 16 + b * 8) * 4 + v.x, my * 8 * 4 + v.y, 8, r, avg);
      } else {
        mc_half(d, cur.stride[0] * 2, luma, mx * 32 + v.x, my * 16 + v.y, 16, 8, r, avg);
      }
      const int cx = chroma_component(v.x);
      const int cy = chroma_component(v.y);
      for (int p = 1; p < 3; ++p) {
        mc_half(cur.at(p, mx * 8, my * 8 + f), cur.stride[static_cast<std::size_t>(p)] * 2, view(pic, p, sel),
                mx * 16 + cx, my * 8 + cy, 8, 4, r, avg);
      }
    }
  }

  /// Global motion compensation with one warping point.
  void predict_gmc(int mx, int my) {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    const Picture& pic = pictures_[static_cast<std::size_t>(forward_)];
    const int accuracy = warp_accuracy_ + 1;  // log2 s
    for (int p = 0; p < 3; ++p) {
      const int n = p == 0 ? 16 : 8;
      int ox = gmc_offset_.x;
      int oy = gmc_offset_.y;
      if (p != 0) {
        ox = (ox >> 1) | (ox & 1);
        oy = (oy >> 1) | (oy & 1);
      }
      const int x0 = mx * n + (ox >> accuracy);
      const int y0 = my * n + (oy >> accuracy);
      const int fx = (ox << (4 - accuracy)) & 15;  // 1/16 sample fraction
      const int fy = (oy << (4 - accuracy)) & 15;
      std::uint8_t src[17 * 17];
      fetch(view(pic, p), x0, y0, n + 1, n + 1, src);
      const int rounder = 128 - rounding_;
      for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
          const std::uint8_t* s = src + y * (n + 1) + x;
          const int v = ((16 - fx) * (16 - fy) * s[0] + fx * (16 - fy) * s[1] + (16 - fx) * fy * s[n + 1] +
                         fx * fy * s[n + 2] + rounder) >> 8;
          *cur.at(p, mx * n + x, my * n + y) = static_cast<std::uint8_t>(v);
        }
      }
    }
  }

  /// The vector a GMC macroblock contributes to motion vector prediction.
  [[nodiscard]] Vec gmc_vector() const {
    const int accuracy = warp_accuracy_ + 1;
    const int unit = quarter_sample_ ? 2 : 1;  // log2 of samples per vector unit inverse
    const int shift = accuracy - unit;
    const auto conv = [&](int v) { return shift >= 0 ? rounded_shift(v, shift) : v * (1 << -shift); };
    return {conv(gmc_offset_.x), conv(gmc_offset_.y)};
  }
  [[nodiscard]] static int rounded_shift(int v, int shift) {
    if (shift == 0) return v;
    return v >= 0 ? (v + (1 << (shift - 1))) >> shift : -((-v + (1 << (shift - 1))) >> shift);
  }

  // ---- blocks --------------------------------------------------------------------------------

  /// Decodes the TCOEF events of one block into `qf` (raster order) along `scan`,
  /// starting at scan position `start`.
  bool decode_coefficients(BitReader& br, bool intra_table, const std::uint8_t* scan, int start,
                           std::array<int, 64>& qf) {
    const Vlc& table = intra_table ? tables_.tcoef_intra : tables_.tcoef_inter;
    const auto& lmax = intra_table ? tables_.lmax_intra : tables_.lmax_inter;
    const auto& rmax = intra_table ? tables_.rmax_intra : tables_.rmax_inter;
    int i = start;
    while (true) {
      int sym = table.decode(br);
      int last;
      int run;
      int level;
      if (sym == kEscape) {
        if (short_header_) {
          if (flv_version_ == 1) {
            const bool is11 = br.bit();
            last = br.bit() ? 1 : 0;
            run = static_cast<int>(br.bits(6));
            level = br.sbits(is11 ? 11 : 7);
          } else {
            last = br.bit() ? 1 : 0;
            run = static_cast<int>(br.bits(6));
            level = br.sbits(8);
            if (level == -128) return false;
          }
          if (level == 0) return false;
        } else if (!br.bit()) {
          // Type 1: level offset.
          sym = table.decode(br);
          if (sym == Vlc::invalid || sym == kEscape) return false;
          last = sym >> 12;
          run = (sym >> 6) & 63;
          level = (sym & 63) + lmax[static_cast<std::size_t>(last)][static_cast<std::size_t>(run)];
          if (br.bit()) level = -level;
        } else if (!br.bit()) {
          // Type 2: run offset.
          sym = table.decode(br);
          if (sym == Vlc::invalid || sym == kEscape) return false;
          last = sym >> 12;
          level = sym & 63;
          run = ((sym >> 6) & 63) + rmax[static_cast<std::size_t>(last)][static_cast<std::size_t>(level)] + 1;
          if (br.bit()) level = -level;
        } else {
          // Type 3: fixed length.
          last = br.bit() ? 1 : 0;
          run = static_cast<int>(br.bits(6));
          br.skip(1);
          level = br.sbits(12);
          br.skip(1);
          if (level == 0) return false;
        }
      } else if (sym == Vlc::invalid) {
        return false;
      } else {
        last = sym >> 12;
        run = (sym >> 6) & 63;
        level = sym & 63;
        if (br.bit()) level = -level;
      }
      i += run;
      if (i > 63 || br.overrun()) return false;
      qf[scan[i]] = level;
      ++i;
      if (last != 0) return true;
    }
  }

  [[nodiscard]] BlockPred& block_pred(int mx, int my, int n) {
    if (n < 4) {
      return luma_pred_[static_cast<std::size_t>((my * 2 + (n >> 1)) * (mb_width_ * 2) + mx * 2 + (n & 1))];
    }
    return chroma_pred_[static_cast<std::size_t>((my * mb_width_ + mx) * 2 + (n - 4))];
  }
  /// Prediction data of a neighbour block, or nullptr when unavailable.
  [[nodiscard]] const BlockPred* neighbour(int mx, int my, int n, int dx, int dy) {
    if (n < 4) {
      int bx = mx * 2 + (n & 1) + dx;
      int by = my * 2 + (n >> 1) + dy;
      if (bx < 0 || by < 0 || bx >= mb_width_ * 2) return nullptr;
      const int nmx = bx / 2;
      const int nmy = by / 2;
      if (!available(nmx, nmy)) return nullptr;
      const BlockPred& b = luma_pred_[static_cast<std::size_t>(by * (mb_width_ * 2) + bx)];
      return b.intra ? &b : nullptr;
    }
    const int nmx = mx + dx;
    const int nmy = my + dy;
    if (nmx < 0 || nmy < 0 || nmx >= mb_width_) return nullptr;
    if (!available(nmx, nmy)) return nullptr;
    const BlockPred& b = chroma_pred_[static_cast<std::size_t>((nmy * mb_width_ + nmx) * 2 + (n - 4))];
    return b.intra ? &b : nullptr;
  }

  /// Decodes an intra block (MPEG-4 with AC/DC prediction, or H.263) and
  /// writes the reconstructed samples.
  bool intra_block(BitReader& br, int mx, int my, int n, bool coded, bool ac_pred, bool use_dc_vlc,
                   std::optional<int> partition_dc, std::uint8_t* dst, int stride) {
    std::array<int, 64> qf{};
    std::array<std::int32_t, 64> f{};
    const bool chroma = n >= 4;
    if (short_header_) {
      int dc = static_cast<int>(br.bits(8));
      if (dc == 0 || dc == 128) return false;
      if (dc == 255) dc = 128;
      if (coded && !decode_coefficients(br, false, zigzag, 1, qf)) return false;
      f[0] = dc * 8;
      for (int i = 1; i < 64; ++i) f[static_cast<std::size_t>(i)] = dequant_h263(qf[static_cast<std::size_t>(i)], qp_);
      idct8x8_put(f.data(), dst, stride);
      return true;
    }
    // DC.
    int dc_diff = 0;
    if (partition_dc) {
      dc_diff = *partition_dc;
    } else if (use_dc_vlc) {
      const int size = (chroma ? tables_.dc_chroma : tables_.dc_luma).decode(br);
      if (size == Vlc::invalid) return false;
      if (size > 0) {
        const int v = static_cast<int>(br.bits(size));
        dc_diff = (v >> (size - 1)) != 0 ? v : v - (1 << size) + 1;
        if (size > 8) br.skip(1);
      }
    }
    // Prediction direction from the DC gradients.
    const BlockPred* a = neighbour(mx, my, n, -1, 0);
    const BlockPred* b = neighbour(mx, my, n, -1, -1);
    const BlockPred* c = neighbour(mx, my, n, 0, -1);
    const int fa = a ? a->dc : 1024;
    const int fb = b ? b->dc : 1024;
    const int fc = c ? c->dc : 1024;
    const bool from_above = std::abs(fa - fb) < std::abs(fb - fc);
    const int scaler = dc_scaler(qp_, chroma);
    const BlockPred* source = from_above ? c : a;
    const std::uint8_t* scan = zigzag;
    if (alternate_vertical_scan_) scan = alternate_scan;
    else if (ac_pred) scan = from_above ? kAltHorizontalScan : alternate_scan;
    if (coded && !decode_coefficients(br, true, scan, use_dc_vlc || partition_dc ? 1 : 0, qf)) return false;
    if (!use_dc_vlc && !partition_dc) dc_diff = qf[0];
    const int pred_dc = from_above ? fc : fa;
    int level = dc_diff + rounded_div(pred_dc, scaler);
    int dc = level * scaler;
    dc = std::clamp(dc, -2048, 2047);
    qf[0] = level;
    // AC prediction.
    if (ac_pred && source != nullptr) {
      const int sq = source->qp;
      for (int k = 1; k < 8; ++k) {
        if (from_above) {
          const int pv = source->row[static_cast<std::size_t>(k - 1)];
          qf[static_cast<std::size_t>(k)] += sq == qp_ ? pv : rounded_div(pv * sq, qp_);
        } else {
          const int pv = source->col[static_cast<std::size_t>(k - 1)];
          qf[static_cast<std::size_t>(k * 8)] += sq == qp_ ? pv : rounded_div(pv * sq, qp_);
        }
      }
    }
    // Store the prediction data of this block.
    BlockPred& self = block_pred(mx, my, n);
    self.dc = dc;
    self.qp = qp_;
    self.intra = true;
    for (int k = 1; k < 8; ++k) {
      self.row[static_cast<std::size_t>(k - 1)] = static_cast<std::int16_t>(std::clamp(qf[static_cast<std::size_t>(k)], -32768, 32767));
      self.col[static_cast<std::size_t>(k - 1)] = static_cast<std::int16_t>(std::clamp(qf[static_cast<std::size_t>(k * 8)], -32768, 32767));
    }
    // Inverse quantisation.
    f[0] = dc;
    std::int64_t sum = dc;
    for (int i = 1; i < 64; ++i) {
      const int q = qf[static_cast<std::size_t>(i)];
      const int v = mpeg_quant_ ? dequant_mpeg(q, intra_matrix_[static_cast<std::size_t>(i)], qp_, true) : dequant_h263(q, qp_);
      f[static_cast<std::size_t>(i)] = v;
      sum += v;
    }
    if (mpeg_quant_ && (sum & 1) == 0) f[63] ^= 1;
    idct8x8_put(f.data(), dst, stride);
    return true;
  }

  bool inter_block(BitReader& br, std::uint8_t* dst, int stride) {
    std::array<int, 64> qf{};
    const std::uint8_t* scan = alternate_vertical_scan_ ? alternate_scan : zigzag;
    if (!decode_coefficients(br, false, scan, 0, qf)) return false;
    std::array<std::int32_t, 64> f{};
    std::int64_t sum = 0;
    for (int i = 0; i < 64; ++i) {
      const int q = qf[static_cast<std::size_t>(i)];
      const int v = mpeg_quant_ ? dequant_mpeg(q, inter_matrix_[static_cast<std::size_t>(i)], qp_, false) : dequant_h263(q, qp_);
      f[static_cast<std::size_t>(i)] = v;
      sum += v;
    }
    if (mpeg_quant_ && (sum & 1) == 0) f[63] ^= 1;
    idct8x8_add(f.data(), dst, stride);
    return true;
  }

  /// Destination of block n of macroblock (mx, my), honouring field DCT.
  void block_target(int mx, int my, int n, bool field_dct, std::uint8_t*& dst, int& stride) {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    if (n < 4) {
      const int bx = mx * 16 + (n & 1) * 8;
      if (field_dct) {
        dst = cur.at(0, bx, my * 16 + (n >> 1));
        stride = cur.stride[0] * 2;
      } else {
        dst = cur.at(0, bx, my * 16 + (n >> 1) * 8);
        stride = cur.stride[0];
      }
      return;
    }
    const int p = n - 3;
    dst = cur.at(p, mx * 8, my * 8);
    stride = cur.stride[static_cast<std::size_t>(p)];
  }

  void clear_intra_pred(int mx, int my) {
    for (int n = 0; n < 6; ++n) block_pred(mx, my, n).intra = false;
  }

  // ---- I/P/S macroblocks ---------------------------------------------------------------------

  bool decode_ip_mb(BitReader& br, int mx, int my) {
    MbInfo& info = mb_[static_cast<std::size_t>(my * mb_width_ + mx)];
    info.packet = packet_;
    info.qp = qp_;
    int mcbpc_value;
    if (type_ != kI) {
      if (br.bit()) {  // not_coded
        info.kind = type_ == kS ? kMbGmc : kMbSkipped;
        info.not_coded = true;
        clear_intra_pred(mx, my);
        if (type_ == kS) {
          predict_gmc(mx, my);
          const Vec g = gmc_vector();
          info.mv = {g, g, g, g};
        } else {
          predict_frame_mb(forward_, mx, my, info.mv, false, rounding_, false);
        }
        return true;
      }
    }
    do {
      mcbpc_value = (type_ == kI ? tables_.mcbpc_i : tables_.mcbpc_p).decode(br);
      if (br.overrun()) return false;
    } while (mcbpc_value == kStuffing);
    if (mcbpc_value == Vlc::invalid) return false;
    const int mb_type = mcbpc_value >> 2;
    const int cbpc = mcbpc_value & 3;
    const bool intra = mb_type >= 3;
    if (mb_type == 5 || (short_header_ && mb_type == 2)) return false;  // Annex F / modified quant only
    bool ac_pred = false;
    if (intra && !short_header_) ac_pred = br.bit();
    bool mcsel = false;
    if (type_ == kS && !intra && mb_type != 2) mcsel = br.bit();
    int cbpy = tables_.cbpy.decode(br);
    if (cbpy == Vlc::invalid) return false;
    if (!intra) cbpy ^= 15;
    const bool use_dc_vlc = qp_ < intra_dc_threshold_;
    if (mb_type == 1 || mb_type == 4) {
      static constexpr int kDquant[4] = {-1, -2, 1, 2};
      qp_ = std::clamp(qp_ + kDquant[br.bits(2)], 1, 31);
      info.qp = qp_;
    }
    const int cbp = (cbpy << 2) | cbpc;
    bool field_dct = false;
    bool field_pred = false;
    if (interlaced_ && !short_header_) {
      if (cbp != 0 || intra) field_dct = br.bit();
      if (!intra && !mcsel && mb_type != 2) {
        field_pred = br.bit();
        if (field_pred) {
          info.field_select[0] = br.bit() ? 1 : 0;
          info.field_select[1] = br.bit() ? 1 : 0;
        }
      }
    }

    if (intra) {
      info.kind = kMbIntra;
      info.mv = {};
      for (int n = 0; n < 6; ++n) {
        std::uint8_t* dst;
        int stride;
        block_target(mx, my, n, field_dct, dst, stride);
        const bool coded = (cbp & (32 >> n)) != 0;
        if (!intra_block(br, mx, my, n, coded, ac_pred, use_dc_vlc, std::nullopt, dst, stride)) return false;
      }
      return true;
    }
    clear_intra_pred(mx, my);
    if (mcsel) {
      info.kind = kMbGmc;
      const Vec g = gmc_vector();
      info.mv = {g, g, g, g};
      predict_gmc(mx, my);
    } else if (field_pred) {
      info.kind = kMbInter;
      info.field = true;
      const Vec pred = predict_mv(mx, my, 0, info.mv);
      for (int f = 0; f < 2; ++f) {
        Vec v;
        if (!decode_mv_component(br, fcode_forward_, pred.x, v.x)) return false;
        if (!decode_mv_component(br, fcode_forward_, pred.y / 2, v.y)) return false;
        info.field_mv[static_cast<std::size_t>(f)] = v;
      }
      predict_field_mb(forward_, mx, my, info.field_mv, info.field_select, rounding_, false);
    } else {
      info.kind = kMbInter;
      const bool four = mb_type == 2;
      info.four = four;
      for (int b = 0; b < (four ? 4 : 1); ++b) {
        const Vec pred = predict_mv(mx, my, b, info.mv);
        Vec v;
        if (!decode_mv(br, fcode_forward_, pred, v)) return false;
        info.mv[static_cast<std::size_t>(b)] = v;
        if (!four) info.mv = {v, v, v, v};
      }
      predict_frame_mb(forward_, mx, my, info.mv, four, rounding_, false);
    }
    for (int n = 0; n < 6; ++n) {
      if ((cbp & (32 >> n)) == 0) continue;
      std::uint8_t* dst;
      int stride;
      block_target(mx, my, n, field_dct, dst, stride);
      if (!inter_block(br, dst, stride)) return false;
    }
    return true;
  }

  // ---- B macroblocks -----------------------------------------------------------------------------

  bool decode_b_mb(BitReader& br, int mx, int my) {
    MbInfo& info = mb_[static_cast<std::size_t>(my * mb_width_ + mx)];
    info.packet = packet_;
    info.qp = qp_;
    clear_intra_pred(mx, my);
    const MbInfo& colocated = ref_mb_[static_cast<std::size_t>(my * mb_width_ + mx)];
    if (colocated.not_coded) {
      // The co-located macroblock of the future reference was skipped:
      // nothing is transmitted; forward prediction with a zero vector.
      info.kind = kMbSkipped;
      predict_frame_mb(forward_, mx, my, {}, false, 0, false);
      return true;
    }
    const int modb = tables_.modb.decode(br);
    if (modb == Vlc::invalid) return false;
    int mb_type = kBDirect;
    int cbp = 0;
    if (modb != 0) {
      mb_type = tables_.b_type.decode(br);
      if (mb_type == Vlc::invalid) return false;
      if (modb == 2) cbp = static_cast<int>(br.bits(6));
      if (mb_type != kBDirect && cbp != 0) {
        if (br.bit()) qp_ = std::clamp(qp_ + (br.bit() ? 2 : -2), 1, 31);
      }
    }
    bool field_dct = false;
    bool field_pred = false;
    std::array<std::uint8_t, 2> fwd_sel{};
    std::array<std::uint8_t, 2> bwd_sel{};
    if (interlaced_) {
      if (cbp != 0) field_dct = br.bit();
      if (mb_type != kBDirect) {
        field_pred = br.bit();
        if (field_pred) {
          if (mb_type != kBBackward) {
            fwd_sel[0] = br.bit() ? 1 : 0;
            fwd_sel[1] = br.bit() ? 1 : 0;
          }
          if (mb_type != kBForward) {
            bwd_sel[0] = br.bit() ? 1 : 0;
            bwd_sel[1] = br.bit() ? 1 : 0;
          }
        }
      }
    }
    info.kind = kMbInter;
    if (mb_type == kBDirect) {
      if (colocated.field) return false;  // field direct mode is not supported
      Vec delta;  // modb "1": direct mode without a vector delta
      if (modb != 0 && !decode_mv(br, 1, Vec{}, delta)) return false;
      const int trb = pb_time_;
      const int trd = pp_time_ == 0 ? 1 : pp_time_;
      std::array<Vec, 4> fwd{};
      std::array<Vec, 4> bwd{};
      const bool four = colocated.four;
      for (int b = 0; b < 4; ++b) {
        const Vec col = colocated.kind == kMbIntra ? Vec{} : colocated.mv[static_cast<std::size_t>(b)];
        Vec& f = fwd[static_cast<std::size_t>(b)];
        Vec& k = bwd[static_cast<std::size_t>(b)];
        f.x = trb * col.x / trd + delta.x;
        f.y = trb * col.y / trd + delta.y;
        k.x = delta.x == 0 ? (trb - trd) * col.x / trd : f.x - col.x;
        k.y = delta.y == 0 ? (trb - trd) * col.y / trd : f.y - col.y;
      }
      predict_frame_mb(forward_, mx, my, fwd, four, 0, false);
      predict_frame_mb(backward_, mx, my, bwd, four, 0, true);
      info.mv = fwd;
    } else if (field_pred) {
      info.field = true;
      std::array<Vec, 2> fwd{};
      std::array<Vec, 2> bwd{};
      if (mb_type != kBBackward) {
        for (int f = 0; f < 2; ++f) {
          Vec v;
          if (!decode_mv_component(br, fcode_forward_, pmv_fwd_.x, v.x)) return false;
          if (!decode_mv_component(br, fcode_forward_, pmv_fwd_.y / 2, v.y)) return false;
          fwd[static_cast<std::size_t>(f)] = v;
        }
        pmv_fwd_ = {fwd[1].x, fwd[1].y * 2};
      }
      if (mb_type != kBForward) {
        for (int f = 0; f < 2; ++f) {
          Vec v;
          if (!decode_mv_component(br, fcode_backward_, pmv_bwd_.x, v.x)) return false;
          if (!decode_mv_component(br, fcode_backward_, pmv_bwd_.y / 2, v.y)) return false;
          bwd[static_cast<std::size_t>(f)] = v;
        }
        pmv_bwd_ = {bwd[1].x, bwd[1].y * 2};
      }
      bool have = false;
      if (mb_type != kBBackward) {
        predict_field_mb(forward_, mx, my, fwd, fwd_sel, 0, false);
        have = true;
      }
      if (mb_type != kBForward) predict_field_mb(backward_, mx, my, bwd, bwd_sel, 0, have);
    } else {
      Vec fwd;
      Vec bwd;
      if (mb_type != kBBackward) {
        if (!decode_mv(br, fcode_forward_, pmv_fwd_, fwd)) return false;
        pmv_fwd_ = fwd;
      }
      if (mb_type != kBForward) {
        if (!decode_mv(br, fcode_backward_, pmv_bwd_, bwd)) return false;
        pmv_bwd_ = bwd;
      }
      bool have = false;
      if (mb_type != kBBackward) {
        predict_frame_mb(forward_, mx, my, {fwd, fwd, fwd, fwd}, false, 0, false);
        have = true;
      }
      if (mb_type != kBForward) predict_frame_mb(backward_, mx, my, {bwd, bwd, bwd, bwd}, false, 0, have);
    }
    for (int n = 0; n < 6; ++n) {
      if ((cbp & (32 >> n)) == 0) continue;
      std::uint8_t* dst;
      int stride;
      block_target(mx, my, n, field_dct, dst, stride);
      if (!inter_block(br, dst, stride)) return false;
    }
    return true;
  }

  // ---- data partitioning -------------------------------------------------------------------------

  struct PartitionedMb {
    int mb = 0;
    int mb_type = 0;
    int cbpc = 0;
    int cbp = 0;
    int qp = 1;
    bool skipped = false;
    bool ac_pred = false;
    bool use_dc_vlc = false;
    std::array<int, 6> dc{};
  };

  void decode_partitioned(BitReader& br) {
    const int count = mb_width_ * mb_height_;
    int mb = 0;
    if (at_resync(br)) mb = video_packet_header(br);
    while (mb >= 0 && mb < count) {
      const int start = mb;
      decode_partitioned_packet(br, mb);
      const int next = next_packet(br);
      if (next <= start || next >= count) return;
      mb = next;
    }
  }

  /// Decodes one data-partitioned video packet starting at macroblock `mb`; advances `mb`.
  bool decode_partitioned_packet(BitReader& br, int& mb) {
    const int count = mb_width_ * mb_height_;
    std::vector<PartitionedMb> list;
    const std::uint32_t marker = type_ == kI ? 0x6B001u : 0x1F001u;  // dc_marker / motion_marker
    const int marker_bits = type_ == kI ? 19 : 17;
    // Partition 1: modes (and motion vectors or DCs).
    for (int at = mb; at < count; ++at) {
      if (br.peek(marker_bits) == marker) break;
      const int mx = at % mb_width_;
      const int my = at / mb_width_;
      MbInfo& info = mb_[static_cast<std::size_t>(at)];
      info.packet = packet_;
      PartitionedMb p;
      p.mb = at;
      if (type_ != kI && br.bit()) {
        p.skipped = true;
        info.kind = kMbSkipped;
        info.not_coded = true;
        info.qp = qp_;
        list.push_back(p);
        continue;
      }
      int value;
      do {
        value = (type_ == kI ? tables_.mcbpc_i : tables_.mcbpc_p).decode(br);
        if (br.overrun()) return false;
      } while (value == kStuffing);
      if (value == Vlc::invalid) return false;
      p.mb_type = value >> 2;
      p.cbpc = value & 3;
      if (p.mb_type > 4) return false;
      const bool intra = p.mb_type >= 3;
      p.use_dc_vlc = qp_ < intra_dc_threshold_;
      if (intra) {
        if (type_ == kI && (p.mb_type == 4)) {
          static constexpr int kDquant[4] = {-1, -2, 1, 2};
          qp_ = std::clamp(qp_ + kDquant[br.bits(2)], 1, 31);
        }
        if (type_ == kI && p.use_dc_vlc) {
          for (int n = 0; n < 6; ++n) {
            const int size = (n >= 4 ? tables_.dc_chroma : tables_.dc_luma).decode(br);
            if (size == Vlc::invalid) return false;
            int diff = 0;
            if (size > 0) {
              const int v = static_cast<int>(br.bits(size));
              diff = (v >> (size - 1)) != 0 ? v : v - (1 << size) + 1;
              if (size > 8) br.skip(1);
            }
            p.dc[static_cast<std::size_t>(n)] = diff;
          }
        }
        info.kind = kMbIntra;
      } else {
        info.kind = kMbInter;
        const bool four = p.mb_type == 2;
        info.four = four;
        for (int b = 0; b < (four ? 4 : 1); ++b) {
          const Vec pred = predict_mv(mx, my, b, info.mv);
          Vec v;
          if (!decode_mv(br, fcode_forward_, pred, v)) return false;
          info.mv[static_cast<std::size_t>(b)] = v;
          if (!four) info.mv = {v, v, v, v};
        }
      }
      p.qp = qp_;
      info.qp = qp_;
      list.push_back(p);
    }
    if (br.bits(marker_bits) != marker) return false;
    // Partition 2: ac_pred/cbpy/dquant (and DCs in P-VOPs).
    for (PartitionedMb& p : list) {
      if (p.skipped) continue;
      const bool intra = p.mb_type >= 3;
      if (type_ == kI) {
        p.ac_pred = br.bit();
        int cbpy = tables_.cbpy.decode(br);
        if (cbpy == Vlc::invalid) return false;
        p.cbp = (cbpy << 2) | p.cbpc;
        continue;
      }
      if (intra) p.ac_pred = br.bit();
      int cbpy = tables_.cbpy.decode(br);
      if (cbpy == Vlc::invalid) return false;
      if (!intra) cbpy ^= 15;
      p.cbp = (cbpy << 2) | p.cbpc;
      p.use_dc_vlc = qp_ < intra_dc_threshold_;  // running quantiser before this dquant
      if (p.mb_type == 1 || p.mb_type == 4) {
        static constexpr int kDquant[4] = {-1, -2, 1, 2};
        qp_ = std::clamp(qp_ + kDquant[br.bits(2)], 1, 31);
      }
      p.qp = qp_;
      mb_[static_cast<std::size_t>(p.mb)].qp = qp_;
      if (intra && p.use_dc_vlc) {
        for (int n = 0; n < 6; ++n) {
          const int size = (n >= 4 ? tables_.dc_chroma : tables_.dc_luma).decode(br);
          if (size == Vlc::invalid) return false;
          int diff = 0;
          if (size > 0) {
            const int v = static_cast<int>(br.bits(size));
            diff = (v >> (size - 1)) != 0 ? v : v - (1 << size) + 1;
            if (size > 8) br.skip(1);
          }
          p.dc[static_cast<std::size_t>(n)] = diff;
        }
      }
    }
    if (rvlc_) return false;  // reversible VLC texture: conceal this packet
    // Partition 3: texture, with motion compensation.
    for (const PartitionedMb& p : list) {
      const int mx = p.mb % mb_width_;
      const int my = p.mb / mb_width_;
      MbInfo& info = mb_[static_cast<std::size_t>(p.mb)];
      qp_ = p.qp;
      if (p.skipped) {
        clear_intra_pred(mx, my);
        predict_frame_mb(forward_, mx, my, info.mv, false, rounding_, false);
        decoded_[static_cast<std::size_t>(p.mb)] = 1;
        continue;
      }
      const bool intra = p.mb_type >= 3;
      if (intra) {
        for (int n = 0; n < 6; ++n) {
          std::uint8_t* dst;
          int stride;
          block_target(mx, my, n, false, dst, stride);
          const bool coded = (p.cbp & (32 >> n)) != 0;
          const std::optional<int> dc = p.use_dc_vlc ? std::optional<int>(p.dc[static_cast<std::size_t>(n)]) : std::nullopt;
          if (!intra_block(br, mx, my, n, coded, p.ac_pred, p.use_dc_vlc, dc, dst, stride)) return false;
        }
      } else {
        clear_intra_pred(mx, my);
        predict_frame_mb(forward_, mx, my, info.mv, p.mb_type == 2, rounding_, false);
        for (int n = 0; n < 6; ++n) {
          if ((p.cbp & (32 >> n)) == 0) continue;
          std::uint8_t* dst;
          int stride;
          block_target(mx, my, n, false, dst, stride);
          if (!inter_block(br, dst, stride)) return false;
        }
      }
      decoded_[static_cast<std::size_t>(p.mb)] = 1;
    }
    mb = list.empty() ? mb + 1 : list.back().mb + 1;
    return true;
  }

  static constexpr int kDcThreshold[8] = {99, 13, 15, 17, 19, 21, 23, 0};

  Tables tables_;
  Flavor flavor_;
  CodecParams params_;

  // Configuration.
  bool vol_seen_ = false;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  int mb_width_ = 0;
  int mb_height_ = 0;
  int time_resolution_ = 25;
  int time_bits_ = 5;
  int fixed_increment_ = 0;
  bool low_delay_ = false;
  bool interlaced_ = false;
  int sprite_ = 0;
  int warp_points_ = 0;
  int warp_accuracy_ = 0;
  bool mpeg_quant_ = false;
  std::array<std::uint8_t, 64> intra_matrix_{};
  std::array<std::uint8_t, 64> inter_matrix_{};
  bool quarter_sample_ = false;
  bool resync_disabled_ = true;
  bool data_partitioned_ = false;
  bool rvlc_ = false;
  bool packed_ = false;
  bool full_range_ = false;
  int matrix_ = 2;
  int last_h263_format_ = 0;
  std::uint32_t custom_w_ = 0;
  std::uint32_t custom_h_ = 0;
  bool plus_umv_ = false;
  bool custom_pcf_ = false;
  bool slice_structured_ = false;

  // Picture.
  int type_ = kI;
  bool short_header_ = false;
  int flv_version_ = -1;
  bool disposable_ = false;
  bool umv_ = false;
  int rounding_ = 0;
  int intra_dc_threshold_ = 99;
  bool alternate_vertical_scan_ = false;
  int qp_ = 1;
  int fcode_forward_ = 1;
  int fcode_backward_ = 1;
  Vec gmc_offset_;
  std::int64_t time_base_ = 0;
  std::int64_t last_time_base_ = 0;
  std::int64_t last_non_b_time_ = 0;
  int pp_time_ = 1;
  int pb_time_ = 0;
  int packet_ = 0;
  Vec pmv_fwd_;
  Vec pmv_bwd_;

  // Buffers.
  std::array<Picture, 3> pictures_;
  int forward_ = -1;
  int backward_ = -1;
  int current_ = 0;
  std::vector<MbInfo> mb_;
  std::vector<MbInfo> ref_mb_;
  std::vector<BlockPred> luma_pred_;
  std::vector<BlockPred> chroma_pred_;
  std::vector<std::uint8_t> decoded_;

  // Output.
  std::multiset<std::int64_t> timestamps_;
  std::optional<VideoFrame> held_;
  std::deque<VideoFrame> output_;
  bool have_last_ = false;
  std::int64_t last_pts_ = 0;
  std::int64_t last_duration_ = 40;
  std::int64_t packet_duration_ms_ = 0;
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_decoder(Flavor flavor) { return std::make_unique<Mpeg4Decoder>(flavor); }

}  // namespace cvt::legacy::mpeg4
