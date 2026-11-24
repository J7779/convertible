// Convertible core - codec_legacy family: MPEG-1 (ISO/IEC 11172-2) and
// MPEG-2 (ISO/IEC 13818-2) video decoder.
//
// Accepts the elementary stream in packets of any size (one picture per
// packet as in MP4/Matroska, or arbitrary PES payloads): the stream is
// re-assembled and split at start codes. A packet's timestamp belongs to the
// first picture whose start code begins inside that packet; pictures without
// one get the previous output time plus the frame duration.
//
// Supported: I/P/B (and MPEG-1 D) pictures, every macroblock type, frame and
// field pictures, frame/field/dual-prime/16x8 motion compensation, field
// DCT, 4:2:0/4:2:2/4:4:4, custom and chroma quantiser matrices, linear and
// non-linear quantiser scale, both intra VLC tables, alternate scan,
// concealment vectors. Damaged slices are concealed from the reference.
// Pan-scan, scalable extensions and the display extension's size are ignored.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

#include "families/codec_legacy/common/bitstream.hpp"
#include "families/codec_legacy/common/codec_base.hpp"
#include "families/codec_legacy/common/dct.hpp"
#include "families/codec_legacy/common/vlc.hpp"
#include "families/codec_legacy/mpeg12/mpeg12.hpp"
#include "families/codec_legacy/mpeg12/mpeg12_common.hpp"
#include "families/codec_legacy/mpeg12/mpeg12_tables.hpp"

namespace cvt::legacy::mpeg12 {

namespace {

constexpr int kTypeI = 1;
constexpr int kTypeP = 2;
constexpr int kTypeB = 3;
constexpr int kTypeD = 4;

constexpr int kTopField = 1;
constexpr int kBottomField = 2;
constexpr int kFramePicture = 3;

// Motion types (frame_motion_type / field_motion_type).
constexpr int kMcField = 1;   // field prediction (frame pictures) / field (field pictures)
constexpr int kMcFrame = 2;   // frame prediction (frame pictures) / 16x8 (field pictures)
constexpr int kMcDualPrime = 3;

struct Tables {
  Vlc mba{kMbaCodes};
  Vlc mb_i{kMbTypeI};
  Vlc mb_p{kMbTypeP};
  Vlc mb_b{kMbTypeB};
  Vlc mb_d{kMbTypeD};
  Vlc cbp{kCbpCodes};
  Vlc motion{kMotionCodes};
  Vlc dmv{kDmvCodes};
  Vlc dc_luma{kDcLumaCodes};
  Vlc dc_chroma{kDcChromaCodes};
  Vlc dct0{kDctZero, 10};
  Vlc dct1{kDctOne, 10};
};

/// A decoded picture, macroblock aligned.
struct Picture {
  std::array<std::vector<std::uint8_t>, 3> plane;
  std::array<int, 3> stride{};
  std::array<int, 3> rows{};
  bool valid = false;

  void allocate(int luma_w, int luma_h, int chroma_w, int chroma_h) {
    stride = {luma_w, chroma_w, chroma_w};
    rows = {luma_h, chroma_h, chroma_h};
    for (int p = 0; p < 3; ++p) {
      plane[p].assign(static_cast<std::size_t>(stride[p]) * static_cast<std::size_t>(rows[p]), p == 0 ? 16 : 128);
    }
    valid = false;
  }
};


struct PendingTimestamp {
  std::uint64_t offset;  // stream offset of the first byte of the packet
  std::int64_t pts;
  std::int64_t duration;
  bool used;
};

class Mpeg12Decoder final : public codec::VideoDecoder {
 public:
  Mpeg12Decoder() = default;

  Result<void> configure(const CodecParams& params) override {
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 1000;
    if (!params.extradata.empty()) {
      // Sequence header (+ extensions) stored out of band: parse it as stream data.
      const auto* data = reinterpret_cast<const std::uint8_t*>(params.extradata.data());
      buffer_.assign(data, data + params.extradata.size());
      CVT_TRY(process(false, true));
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(scan_));
      base_ += scan_;
      scan_ = 0;
    }
    return {};
  }

  Result<void> send_packet(const Packet& packet) override {
    if (packet.bytes.empty()) return {};
    // INT64_MIN marks a packet without a timestamp (PES packets without a PTS).
    timestamps_.push_back({base_ + buffer_.size(), packet.pts, packet.duration, packet.pts == INT64_MIN});
    const auto* data = reinterpret_cast<const std::uint8_t*>(packet.bytes.data());
    buffer_.insert(buffer_.end(), data, data + packet.bytes.size());
    return process(false, false);
  }

  Result<std::optional<VideoFrame>> receive_frame() override {
    if (output_.empty()) return std::optional<VideoFrame>{};
    VideoFrame frame = std::move(output_.front());
    output_.pop_front();
    return std::optional<VideoFrame>(std::move(frame));
  }

  Result<void> flush() override {
    CVT_TRY(process(true, false));
    if (held_) {
      emit(std::move(*held_));
      held_.reset();
    }
    return {};
  }

 private:
  // ---- stream assembly ------------------------------------------------------

  static constexpr std::size_t npos = SIZE_MAX;

  /// Offset of the next complete start code (00 00 01 xx) at or after `from`, or npos.
  [[nodiscard]] std::size_t find_start_code(std::size_t from) const {
    const std::size_t n = buffer_.size();
    for (std::size_t i = from; i + 3 < n; ++i) {
      if (buffer_[i + 2] > 1) {
        i += 2;
        continue;
      }
      if (buffer_[i] == 0 && buffer_[i + 1] == 0 && buffer_[i + 2] == 1) return i;
    }
    return npos;
  }

  /// Decodes every complete syntax unit in the buffer. A picture is complete
  /// once the start code after its last slice is present (or at the end).
  Result<void> process(bool final, bool headers_only) {
    while (true) {
      const std::size_t at = find_start_code(scan_);
      if (at == npos) {
        if (final || headers_only) scan_ = buffer_.size();
        break;
      }
      const std::uint8_t code = buffer_[at + 3];
      if (code == kPictureStart && !headers_only) {
        // Find the end of the picture: the next picture/sequence/GOP/end code.
        std::size_t end = at + 4;
        std::size_t next = npos;
        while (true) {
          next = find_start_code(end);
          if (next == npos) break;
          const std::uint8_t c = buffer_[next + 3];
          if (c == kPictureStart || c == kSequenceHeader || c == kGroupStart || c == kSequenceEnd) break;
          end = next + 4;
        }
        if (next == npos && !final) {
          scan_ = at;
          break;
        }
        const std::size_t stop = next == npos ? buffer_.size() : next;
        CVT_TRY(decode_picture(at, stop));
        scan_ = stop;
        continue;
      }
      // Header units extend to the next start code.
      const std::size_t next = find_start_code(at + 4);
      if (next == npos && !final && !headers_only) {
        scan_ = at;
        break;
      }
      const std::size_t stop = next == npos ? buffer_.size() : next;
      CVT_TRY(decode_header(code, at + 4, stop));
      scan_ = stop;
    }
    // Drop consumed bytes (keep the buffer small).
    if (scan_ > 0 && !headers_only) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(scan_));
      base_ += scan_;
      scan_ = 0;
    }
    return {};
  }

  [[nodiscard]] std::span<const std::uint8_t> span(std::size_t from, std::size_t to) const {
    return {buffer_.data() + from, to - from};
  }

  // ---- headers ------------------------------------------------------------------

  Result<void> decode_header(std::uint8_t code, std::size_t from, std::size_t to) {
    BitReader br(span(from, to));
    switch (code) {
      case kSequenceHeader: return sequence_header(br);
      case kExtension: return extension(br);
      case kGroupStart:
        br.skip(25);  // time_code
        closed_gop_ = br.bit();
        broken_link_ = br.bit();
        if (broken_link_) drop_b_ = true;
        return {};
      case kSequenceEnd:
      case kUserData:
      default: return {};
    }
  }

  static void read_matrix(BitReader& br, std::array<std::uint8_t, 64>& m) {
    for (int i = 0; i < 64; ++i) {
      const auto v = static_cast<std::uint8_t>(br.bits(8));
      m[zigzag[i]] = v == 0 ? 1 : v;  // 0 is forbidden; keep arithmetic sane
    }
  }

  Result<void> sequence_header(BitReader& br) {
    const std::uint32_t w = br.bits(12);
    const std::uint32_t h = br.bits(12);
    br.skip(4);  // aspect_ratio_information
    frame_rate_code_ = static_cast<int>(br.bits(4));
    br.skip(18 + 1 + 10 + 1);  // bit_rate, marker, vbv_buffer_size, constrained_parameters_flag
    std::array<std::uint8_t, 64> intra{};
    std::array<std::uint8_t, 64> inter{};
    if (br.bit()) {
      read_matrix(br, intra);
    } else {
      std::copy(std::begin(kDefaultIntraMatrix), std::end(kDefaultIntraMatrix), intra.begin());
    }
    if (br.bit()) {
      read_matrix(br, inter);
    } else {
      inter.fill(16);
    }
    if (br.overrun()) return fail(Status::Decode, "truncated MPEG video sequence header");
    if (w == 0 || h == 0) return fail(Status::Decode, "MPEG video sequence header has no picture size");
    intra_matrix_ = chroma_intra_matrix_ = intra;
    inter_matrix_ = chroma_inter_matrix_ = inter;
    // A sequence header without a following sequence extension is MPEG-1;
    // the extension (if any) switches to MPEG-2 and may extend the size.
    mpeg2_ = false;
    chroma_format_ = 1;
    progressive_sequence_ = true;
    frame_rate_ext_n_ = 0;
    frame_rate_ext_d_ = 0;
    low_delay_ = false;
    base_width_ = w;
    base_height_ = h;
    return set_size(w, h);
  }

  Result<void> set_size(std::uint32_t w, std::uint32_t h) {
    CVT_TRY(check_dimensions(w, h));
    width_ = w;
    height_ = h;
    return {};
  }

  /// (Re)allocates the picture buffers when the sequence layout changed.
  void apply_layout() {
    const std::array<std::uint32_t, 4> layout = {width_, height_, static_cast<std::uint32_t>(chroma_format_),
                                                 progressive_sequence_ ? 1u : 0u};
    if (layout == allocated_layout_) return;
    allocated_layout_ = layout;
    // A new layout: finish what is pending and start over.
    if (held_) {
      emit(std::move(*held_));
      held_.reset();
    }
    first_field_pending_ = false;
    mb_width_ = static_cast<int>((width_ + 15) / 16);
    mb_height_ = progressive_sequence_ ? static_cast<int>((height_ + 15) / 16) : 2 * static_cast<int>((height_ + 31) / 32);
    const int lw = mb_width_ * 16;
    const int lh = mb_height_ * 16;
    const int cw = chroma_format_ == 3 ? lw : lw / 2;
    const int ch = chroma_format_ == 1 ? lh / 2 : lh;
    for (Picture& p : pictures_) p.allocate(lw, lh, cw, ch);
    forward_ = -1;
    backward_ = -1;
    decoded_.assign(static_cast<std::size_t>(mb_width_) * static_cast<std::size_t>(mb_height_), 0);
  }

  Result<void> extension(BitReader& br) {
    const std::uint32_t id = br.bits(4);
    switch (id) {
      case 1: {  // sequence extension
        br.skip(8);  // profile_and_level_indication
        progressive_sequence_ = br.bit();
        chroma_format_ = static_cast<int>(br.bits(2));
        const std::uint32_t hx = br.bits(2);
        const std::uint32_t vx = br.bits(2);
        br.skip(12 + 1 + 8);  // bit_rate_extension, marker, vbv_buffer_size_extension
        low_delay_ = br.bit();
        frame_rate_ext_n_ = static_cast<int>(br.bits(2));
        frame_rate_ext_d_ = static_cast<int>(br.bits(5));
        if (br.overrun()) return fail(Status::Decode, "truncated MPEG-2 sequence extension");
        if (chroma_format_ == 0) return fail(Status::Decode, "reserved MPEG-2 chroma format");
        mpeg2_ = true;
        return set_size(base_width_ | (hx << 12), base_height_ | (vx << 12));
      }
      case 2: {  // sequence display extension
        br.skip(3);  // video_format
        if (br.bit()) {
          br.skip(8 + 8);  // colour_primaries, transfer_characteristics
          matrix_coefficients_ = static_cast<int>(br.bits(8));
        }
        return {};
      }
      case 3: {  // quant matrix extension
        if (br.bit()) {
          read_matrix(br, intra_matrix_);
          chroma_intra_matrix_ = intra_matrix_;
        }
        if (br.bit()) {
          read_matrix(br, inter_matrix_);
          chroma_inter_matrix_ = inter_matrix_;
        }
        if (br.bit()) read_matrix(br, chroma_intra_matrix_);
        if (br.bit()) read_matrix(br, chroma_inter_matrix_);
        return {};
      }
      case 8: {  // picture coding extension
        if (!picture_header_seen_) return {};
        for (auto& row : f_code_) {
          for (int& f : row) f = static_cast<int>(br.bits(4));
        }
        intra_dc_precision_ = static_cast<int>(br.bits(2));
        picture_structure_ = static_cast<int>(br.bits(2));
        top_field_first_ = br.bit();
        frame_pred_frame_dct_ = br.bit();
        concealment_mv_ = br.bit();
        q_scale_type_ = br.bit();
        intra_vlc_format_ = br.bit();
        alternate_scan_ = br.bit();
        repeat_first_field_ = br.bit();
        br.skip(1);  // chroma_420_type
        progressive_frame_ = br.bit();
        if (br.overrun()) return fail(Status::Decode, "truncated MPEG-2 picture coding extension");
        if (picture_structure_ == 0) return fail(Status::Decode, "reserved MPEG-2 picture structure");
        return {};
      }
      default: return {};
    }
  }

  // ---- pictures ------------------------------------------------------------------

  std::optional<std::int64_t> take_timestamp(std::size_t offset_in_buffer, std::int64_t& duration) {
    const std::uint64_t at = base_ + offset_in_buffer;
    // Drop packets that ended before this picture started.
    while (timestamps_.size() > 1 && timestamps_[1].offset <= at) timestamps_.pop_front();
    if (timestamps_.empty() || timestamps_.front().offset > at || timestamps_.front().used) return std::nullopt;
    timestamps_.front().used = true;
    duration = timestamps_.front().duration;
    return timestamps_.front().pts;
  }

  Result<void> decode_picture(std::size_t at, std::size_t stop) {
    if (width_ == 0) return {};  // no sequence header yet: skip
    // Picture header.
    std::size_t header_end = find_start_code(at + 4);
    if (header_end == npos || header_end > stop) header_end = stop;
    BitReader br(span(at + 4, header_end));
    br.skip(10);  // temporal_reference
    picture_type_ = static_cast<int>(br.bits(3));
    br.skip(16);  // vbv_delay
    full_pel_[0] = full_pel_[1] = false;
    int mpeg1_f[2] = {1, 1};
    if (picture_type_ == kTypeP || picture_type_ == kTypeB) {
      full_pel_[0] = br.bit();
      mpeg1_f[0] = static_cast<int>(br.bits(3));
    }
    if (picture_type_ == kTypeB) {
      full_pel_[1] = br.bit();
      mpeg1_f[1] = static_cast<int>(br.bits(3));
    }
    if (br.overrun() || picture_type_ < kTypeI || picture_type_ > kTypeD) {
      return {};  // damaged picture header: skip the picture
    }
    // Defaults for MPEG-1 (overridden by the picture coding extension).
    picture_header_seen_ = true;
    f_code_[0][0] = f_code_[0][1] = mpeg1_f[0];
    f_code_[1][0] = f_code_[1][1] = mpeg1_f[1];
    intra_dc_precision_ = 0;
    picture_structure_ = kFramePicture;
    top_field_first_ = true;
    frame_pred_frame_dct_ = true;
    concealment_mv_ = false;
    q_scale_type_ = false;
    intra_vlc_format_ = false;
    alternate_scan_ = false;
    repeat_first_field_ = false;
    progressive_frame_ = true;

    // Extensions and slices.
    std::vector<std::pair<std::size_t, std::size_t>> slices;
    std::size_t pos = header_end;
    while (pos < stop) {
      std::size_t next = find_start_code(pos + 4);
      if (next == npos || next > stop) next = stop;
      const std::uint8_t code = buffer_[pos + 3];
      if (code == kExtension || code == kUserData) {
        CVT_TRY(decode_header(code, pos + 4, next));
      } else if (code >= kSliceMin && code <= kSliceMax) {
        slices.emplace_back(pos, next);
      }
      pos = next;
    }
    picture_header_seen_ = false;
    if (!mpeg2_) {
      // MPEG-1 has no picture coding extension: frame pictures only.
      picture_structure_ = kFramePicture;
    }
    apply_layout();

    std::int64_t packet_duration = 0;
    const std::optional<std::int64_t> pts = take_timestamp(at, packet_duration);

    // Second field of a frame?
    const bool field = picture_structure_ != kFramePicture;
    const bool second_field = field && first_field_pending_ && first_field_structure_ != picture_structure_;
    if (!second_field) {
      if (first_field_pending_) {
        // An unpaired field: finish the frame with what we have.
        first_field_pending_ = false;
        CVT_TRY(finish_picture());
      }
      // Choose the buffers.
      if (picture_type_ == kTypeB) {
        // Leading B pictures of an open GOP at the start of the stream (or
        // after a broken link) reference a picture we never saw: drop them.
        // In a closed GOP they only use the backward reference.
        if (backward_ < 0 || ((forward_ < 0 || drop_b_) && !closed_gop_)) return {};
        if (forward_ < 0) forward_ = backward_;
        current_ = 0;
        while (current_ == forward_ || current_ == backward_) ++current_;
      } else {
        drop_b_ = false;
        if (backward_ >= 0) forward_ = backward_;
        int spare = 0;
        while (spare == forward_) ++spare;
        current_ = spare;
        if (forward_ < 0) {
          // A P picture without a preceding I picture (damaged or cut
          // stream): predict from mid-grey.
          forward_ = current_ == 0 ? 1 : 0;
          Picture& grey = pictures_[static_cast<std::size_t>(forward_)];
          for (std::size_t p = 0; p < 3; ++p) std::fill(grey.plane[p].begin(), grey.plane[p].end(), p == 0 ? 16 : 128);
        }
      }
      current_type_ = picture_type_;
      current_pts_ = pts;
      current_duration_ = packet_duration;
      current_rff_ = repeat_first_field_;
      current_tff_ = top_field_first_;
      std::fill(decoded_.begin(), decoded_.end(), 0);
    } else if (picture_type_ == kTypeB && current_type_ != kTypeB) {
      return {};  // a B field cannot complete a reference frame
    }

    for (const auto& [from, to] : slices) decode_slice(from, to, second_field);

    if (field && !second_field) {
      first_field_pending_ = true;
      first_field_structure_ = picture_structure_;
      return {};
    }
    first_field_pending_ = false;
    return finish_picture();
  }

  /// Conceals undecoded macroblocks, then outputs or holds the picture.
  Result<void> finish_picture() {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    const int ref = current_type_ == kTypeB ? forward_ : (current_type_ == kTypeP ? forward_ : -1);
    for (int my = 0; my < mb_height_; ++my) {
      for (int mx = 0; mx < mb_width_; ++mx) {
        if (decoded_[static_cast<std::size_t>(my * mb_width_ + mx)] != 0) continue;
        for (int p = 0; p < 3; ++p) {
          const int bw = p == 0 ? 16 : (chroma_format_ == 3 ? 16 : 8);
          const int bh = p == 0 ? 16 : (chroma_format_ == 1 ? 8 : 16);
          for (int y = 0; y < bh; ++y) {
            std::uint8_t* d = cur.plane[p].data() + static_cast<std::ptrdiff_t>(my * bh + y) * cur.stride[p] + mx * bw;
            if (ref >= 0 && ref != current_) {
              const Picture& r = pictures_[static_cast<std::size_t>(ref)];
              std::memcpy(d, r.plane[p].data() + static_cast<std::ptrdiff_t>(my * bh + y) * r.stride[p] + mx * bw,
                          static_cast<std::size_t>(bw));
            } else {
              std::memset(d, p == 0 ? 16 : 128, static_cast<std::size_t>(bw));
            }
          }
        }
      }
    }
    cur.valid = true;
    VideoFrame frame = make_frame(cur);
    if (current_type_ == kTypeB) {
      emit(std::move(frame));
    } else {
      if (backward_ >= 0 && backward_ != current_) forward_ = backward_;
      backward_ = current_;
      if (held_) emit(std::move(*held_));
      held_ = std::move(frame);
      if (low_delay_) {
        emit(std::move(*held_));
        held_.reset();
      }
    }
    return {};
  }

  [[nodiscard]] ColorInfo color() const {
    ColorInfo c;
    c.range = ir::ColorRange::Limited;
    c.siting = mpeg2_ ? ir::ChromaSiting::Left : ir::ChromaSiting::Center;
    switch (matrix_coefficients_) {
      case 1:
      case 7: c.matrix = ir::ColorMatrix::BT709; break;
      case 4:
      case 5:
      case 6: c.matrix = ir::ColorMatrix::BT601; break;
      default:
        // Not signalled: SD material is BT.601 in practice, HD BT.709.
        c.matrix = (mpeg2_ && height_ >= 720) ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601;
        break;
    }
    return c;
  }

  [[nodiscard]] std::int64_t frame_duration_ms() const {
    const FrameRate r = kFrameRates[frame_rate_code_ & 15];
    if (r.num == 0) return 40;
    const std::int64_t num = r.num * (frame_rate_ext_n_ + 1);
    const std::int64_t den = r.den * (frame_rate_ext_d_ + 1);
    return std::max<std::int64_t>(1, (1000 * den + num / 2) / num);
  }

  [[nodiscard]] VideoFrame make_frame(const Picture& pic) {
    const PixelFormat format =
        chroma_format_ == 1 ? PixelFormat::I420 : (chroma_format_ == 2 ? PixelFormat::I422 : PixelFormat::I444);
    VideoFrame frame = VideoFrame::allocate(format, width_, height_);
    frame.color = color();
    for (std::size_t p = 0; p < 3; ++p) {
      const std::size_t row = ir::plane_row_bytes(format, p, width_);
      const std::size_t rows = ir::plane_height(format, p, height_);
      for (std::size_t y = 0; y < rows; ++y) {
        std::memcpy(frame.planes[p].data.data() + y * frame.planes[p].stride,
                    pic.plane[p].data() + y * static_cast<std::size_t>(pic.stride[p]), row);
      }
    }
    std::int64_t duration = frame_duration_ms();
    if (current_rff_) duration = progressive_sequence_ ? duration * (current_tff_ ? 3 : 2) : duration * 3 / 2;
    if (current_duration_ > 0) duration = ticks_to_ms(current_duration_, params_.timescale);
    frame.duration_ms = duration;
    // Timestamps travel with the frame: -1 marks "unknown".
    frame.pts_ms = current_pts_ ? ticks_to_ms(*current_pts_, params_.timescale) : INT64_MIN;
    return frame;
  }

  void emit(VideoFrame frame) {
    // Unknown or out-of-order times continue from the previous frame.
    if (frame.pts_ms == INT64_MIN || (have_last_pts_ && frame.pts_ms <= last_pts_)) {
      frame.pts_ms = have_last_pts_ ? last_pts_ + last_duration_ : 0;
    }
    have_last_pts_ = true;
    last_pts_ = frame.pts_ms;
    last_duration_ = std::max<std::int64_t>(1, frame.duration_ms);
    output_.push_back(std::move(frame));
  }

  // ---- slices and macroblocks -------------------------------------------------------

  struct MotionState {
    int pmv[2][2][2]{};       // [r][s][t]
    int vector[2][2][2]{};    // [r][s][t] reconstructed (half-sample, field units for field vectors)
    int select[2][2]{};       // motion_vertical_field_select[r][s]
    int dmv[2]{};
    int type = kMcFrame;      // motion type of the last macroblock
    int flags = 0;            // macroblock type of the last macroblock (B skip reuse)
  };

  [[nodiscard]] int quantiser_scale(int code) const {
    if (!mpeg2_) return code;
    return q_scale_type_ ? kNonLinearScale[code] : code * 2;
  }

  void reset_dc() {
    const int v = mpeg2_ ? (1 << (intra_dc_precision_ + 7)) : 128;
    dc_pred_[0] = dc_pred_[1] = dc_pred_[2] = v;
  }

  bool decode_motion_component(BitReader& br, int f_code, int pred, int& out) {
    int code = tables_.motion.decode(br);
    if (code == Vlc::invalid) return false;
    if (code != 0 && br.bit()) code = -code;
    const int r_size = std::clamp(f_code, 1, 9) - 1;
    int delta = code;
    if (r_size != 0 && code != 0) {
      const int residual = static_cast<int>(br.bits(r_size));
      delta = ((std::abs(code) - 1) << r_size) + residual + 1;
      if (code < 0) delta = -delta;
    }
    const int f = 1 << r_size;
    const int high = 16 * f - 1;
    const int low = -16 * f;
    const int range = 32 * f;
    int v = pred + delta;
    if (v < low) v += range;
    if (v > high) v -= range;
    out = v;
    return true;
  }

  /// motion_vector(r, s); `field_format` scales the vertical prediction.
  bool motion_vector(BitReader& br, MotionState& ms, int r, int s, bool field_format, bool dual_prime) {
    for (int t = 0; t < 2; ++t) {
      int pred = ms.pmv[r][s][t];
      if (t == 1 && field_format && picture_structure_ == kFramePicture) pred >>= 1;
      int v = 0;
      if (!decode_motion_component(br, f_code_[s][t], pred, v)) return false;
      if (dual_prime) {
        const int d = tables_.dmv.decode(br);
        if (d == Vlc::invalid) return false;
        ms.dmv[t] = d;
      }
      ms.vector[r][s][t] = v;
      ms.pmv[r][s][t] = (t == 1 && field_format && picture_structure_ == kFramePicture) ? v * 2 : v;
    }
    return true;
  }

  bool motion_vectors(BitReader& br, MotionState& ms, int s, int motion_type) {
    const bool frame_pic = picture_structure_ == kFramePicture;
    if (frame_pic) {
      if (motion_type == kMcFrame) {
        if (!motion_vector(br, ms, 0, s, false, false)) return false;
        ms.pmv[1][s][0] = ms.pmv[0][s][0];
        ms.pmv[1][s][1] = ms.pmv[0][s][1];
        return true;
      }
      if (motion_type == kMcField) {
        for (int r = 0; r < 2; ++r) {
          ms.select[r][s] = br.bit() ? 1 : 0;
          if (!motion_vector(br, ms, r, s, true, false)) return false;
        }
        return true;
      }
      // Dual prime.
      if (!motion_vector(br, ms, 0, s, true, true)) return false;
      ms.pmv[1][s][0] = ms.pmv[0][s][0];
      ms.pmv[1][s][1] = ms.pmv[0][s][1];
      return true;
    }
    if (motion_type == kMcField) {
      ms.select[0][s] = br.bit() ? 1 : 0;
      if (!motion_vector(br, ms, 0, s, true, false)) return false;
      ms.pmv[1][s][0] = ms.pmv[0][s][0];
      ms.pmv[1][s][1] = ms.pmv[0][s][1];
      return true;
    }
    if (motion_type == kMcFrame) {  // 16x8
      for (int r = 0; r < 2; ++r) {
        ms.select[r][s] = br.bit() ? 1 : 0;
        if (!motion_vector(br, ms, r, s, true, false)) return false;
      }
      return true;
    }
    if (!motion_vector(br, ms, 0, s, true, true)) return false;
    ms.pmv[1][s][0] = ms.pmv[0][s][0];
    ms.pmv[1][s][1] = ms.pmv[0][s][1];
    return true;
  }

  /// Parses one block's coefficients into `blk` (dequantised, raster order).
  bool decode_block(BitReader& br, int index, bool intra, int qscale, std::int32_t* blk) {
    const int comp = index < 4 ? 0 : (index & 1) == 0 ? 1 : 2;
    const bool chroma = comp != 0;
    const std::uint8_t* scan = alternate_scan_ ? alternate_scan : zigzag;
    const std::array<std::uint8_t, 64>& w =
        intra ? (chroma ? chroma_intra_matrix_ : intra_matrix_) : (chroma ? chroma_inter_matrix_ : inter_matrix_);
    int i;
    std::int64_t sum = 0;
    const Vlc* table = &tables_.dct0;
    if (intra) {
      const int size = (chroma ? tables_.dc_chroma : tables_.dc_luma).decode(br);
      if (size == Vlc::invalid) return false;
      int diff = 0;
      if (size > 0) {
        const int v = static_cast<int>(br.bits(size));
        diff = (v & (1 << (size - 1))) != 0 ? v : v - (1 << size) + 1;
      }
      dc_pred_[comp] += diff;
      const int dc = mpeg2_ ? dc_pred_[comp] * (1 << (3 - intra_dc_precision_)) : dc_pred_[comp] * 8;
      blk[0] = dc;
      sum = dc;
      i = 0;
      if (mpeg2_ && intra_vlc_format_) table = &tables_.dct1;
      if (picture_type_ == kTypeD) return true;
    } else {
      i = -1;
      // First coefficient: "1s" means run 0, level 1.
      if (br.peek(1) == 1) {
        br.skip(1);
        const int level = br.bit() ? -1 : 1;
        i = 0;
        const int pos = scan[0];
        blk[pos] = dequant_inter(level, w[static_cast<std::size_t>(pos)], qscale);
        sum += blk[pos];
      }
    }
    while (true) {
      const int sym = table->decode(br);
      if (sym == kDctEob) break;
      int run;
      int level;
      if (sym == kDctEscape) {
        run = static_cast<int>(br.bits(6));
        if (mpeg2_) {
          level = br.sbits(12);
          if (level == 0 || level == -2048) return false;
        } else {
          level = br.sbits(8);
          if (level == -128) level = static_cast<int>(br.bits(8)) - 256;
          else if (level == 0) level = static_cast<int>(br.bits(8));
          if (level == 0) return false;
        }
      } else if (sym == Vlc::invalid || sym < 0) {
        return false;
      } else {
        run = sym >> 6;
        level = sym & 63;
        if (br.bit()) level = -level;
      }
      i += run + 1;
      if (i > 63 || br.overrun()) return false;
      const int pos = scan[i];
      const int wv = w[static_cast<std::size_t>(pos)];
      const int value = intra ? dequant_intra(level, wv, qscale) : dequant_inter(level, wv, qscale);
      blk[pos] = value;
      sum += value;
    }
    if (mpeg2_ && (sum & 1) == 0) blk[63] ^= 1;  // mismatch control
    return !br.overrun();
  }

  [[nodiscard]] int dequant_intra(int level, int w, int qscale) const {
    return mpeg12::dequant_intra(mpeg2_, level, w, qscale);
  }
  [[nodiscard]] int dequant_inter(int level, int w, int qscale) const {
    return mpeg12::dequant_inter(mpeg2_, level, w, qscale);
  }

  /// Reference plane `p` of picture `index`, whole frame or field `parity`
  /// (0 top, 1 bottom, -1 frame).
  [[nodiscard]] RefPlane ref_plane(int index, int p, int parity) const {
    const Picture& pic = pictures_[static_cast<std::size_t>(index)];
    RefPlane r;
    r.data = pic.plane[p].data();
    r.stride = pic.stride[p];
    r.width = pic.stride[p];
    r.height = pic.rows[p];
    if (parity >= 0) {
      r.data += parity * pic.stride[p];
      r.stride *= 2;
      r.height /= 2;
    }
    return r;
  }

  [[nodiscard]] int chroma_w() const { return chroma_format_ == 3 ? 16 : 8; }
  [[nodiscard]] int chroma_h() const { return chroma_format_ == 1 ? 8 : 16; }

  /// Predicts one region of all three planes: luma block bw x bh at luma
  /// position (lx, ly) of the destination plane view, vector (vx, vy).
  void predict_region(int ref_index, int ref_parity, int dst_parity, int lx, int ly, int bw, int bh, int vx, int vy,
                      bool average) {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    for (int p = 0; p < 3; ++p) {
      const bool chroma = p != 0;
      const int sx = chroma && chroma_format_ != 3 ? 1 : 0;
      const int sy = chroma && chroma_format_ == 1 ? 1 : 0;
      const int x = lx >> sx;
      const int y = ly >> sy;
      const int w = bw >> sx;
      const int h = bh >> sy;
      // Chroma vectors: luma vector / 2 (truncation towards zero) per subsampled axis.
      const int cvx = sx != 0 ? vx / 2 : vx;
      const int cvy = sy != 0 ? vy / 2 : vy;
      int stride = cur.stride[p];
      std::uint8_t* dst = cur.plane[p].data();
      if (dst_parity >= 0) {
        dst += dst_parity * stride;
        stride *= 2;
      }
      dst += static_cast<std::ptrdiff_t>(y) * stride + x;
      const RefPlane ref = ref_plane(ref_index, p, ref_parity);
      predict(dst, stride, ref, x * 2 + cvx, y * 2 + cvy, w, h, average);
    }
  }

  /// Index of the picture holding the reference field `select` for a field
  /// picture (`backward` = backward reference of a B field picture).
  [[nodiscard]] int field_reference(int select, bool backward, bool second_field) const {
    if (backward) return backward_;
    const int current_parity = picture_structure_ == kBottomField ? 1 : 0;
    if (picture_type_ == kTypeP && second_field && select != current_parity) return current_;
    return forward_;
  }

  /// Motion compensation of macroblock (mx, my) for direction s.
  void motion_compensate(const MotionState& ms, int s, int motion_type, int mx, int my, bool average,
                         bool second_field) {
    const int ref = s == 0 ? forward_ : backward_;
    if (picture_structure_ == kFramePicture) {
      const int lx = mx * 16;
      const int ly = my * 16;
      if (motion_type == kMcFrame) {
        const int vx = ms.vector[0][s][0] << (full_pel_[s] ? 1 : 0);
        const int vy = ms.vector[0][s][1] << (full_pel_[s] ? 1 : 0);
        predict_region(ref, -1, -1, lx, ly, 16, 16, vx, vy, average);
      } else if (motion_type == kMcField) {
        for (int r = 0; r < 2; ++r) {
          predict_region(ref, ms.select[r][s], r, lx, ly / 2, 16, 8, ms.vector[r][s][0], ms.vector[r][s][1], average);
        }
      } else {
        // Dual prime (P pictures only): same parity + opposite parity average.
        const int vx = ms.vector[0][0][0];
        const int vy = ms.vector[0][0][1];
        const int m_top = top_field_first_ ? 1 : 3;     // top field predicted from the bottom field
        const int m_bottom = top_field_first_ ? 3 : 1;  // bottom field predicted from the top field
        for (int r = 0; r < 2; ++r) {
          const int m = r == 0 ? m_top : m_bottom;
          const int ox = div2_round(vx * m) + ms.dmv[0];
          const int oy = div2_round(vy * m) + ms.dmv[1] + (r == 0 ? -1 : 1);
          predict_region(ref, r, r, lx, ly / 2, 16, 8, vx, vy, false);
          predict_region(ref, 1 - r, r, lx, ly / 2, 16, 8, ox, oy, true);
        }
      }
      return;
    }
    // Field pictures.
    const int parity = picture_structure_ == kBottomField ? 1 : 0;
    const int lx = mx * 16;
    const int ly = my * 16;
    if (motion_type == kMcField) {
      const int sel = ms.select[0][s];
      predict_region(field_reference(sel, s == 1, second_field), sel, parity, lx, ly, 16, 16, ms.vector[0][s][0],
                     ms.vector[0][s][1], average);
    } else if (motion_type == kMcFrame) {  // 16x8
      for (int r = 0; r < 2; ++r) {
        const int sel = ms.select[r][s];
        predict_region(field_reference(sel, s == 1, second_field), sel, parity, lx, ly + 8 * r, 16, 8,
                       ms.vector[r][s][0], ms.vector[r][s][1], average);
      }
    } else {
      const int vx = ms.vector[0][0][0];
      const int vy = ms.vector[0][0][1];
      const int ox = div2_round(vx) + ms.dmv[0];
      const int oy = div2_round(vy) + ms.dmv[1] + (parity == 0 ? -1 : 1);
      predict_region(field_reference(parity, false, second_field), parity, parity, lx, ly, 16, 16, vx, vy, false);
      predict_region(field_reference(1 - parity, false, second_field), 1 - parity, parity, lx, ly, 16, 16, ox, oy,
                     true);
    }
    (void)ref;
  }

  /// Zero-vector prediction of a skipped P macroblock.
  void predict_skipped_p(int mx, int my, bool second_field) {
    if (picture_structure_ == kFramePicture) {
      predict_region(forward_, -1, -1, mx * 16, my * 16, 16, 16, 0, 0, false);
    } else {
      const int parity = picture_structure_ == kBottomField ? 1 : 0;
      predict_region(field_reference(parity, false, second_field), parity, parity, mx * 16, my * 16, 16, 16, 0, 0,
                     false);
    }
  }

  void predict_inter(const MotionState& ms, int flags, int motion_type, int mx, int my, bool second_field) {
    if (picture_type_ == kTypeP && (flags & kMbForward) == 0) {
      // P macroblock without motion compensation: zero vector, frame/field as the picture.
      predict_skipped_p(mx, my, second_field);
      return;
    }
    bool have = false;
    if ((flags & kMbForward) != 0 && forward_ >= 0) {
      motion_compensate(ms, 0, motion_type, mx, my, false, second_field);
      have = true;
    }
    if ((flags & kMbBackward) != 0 && backward_ >= 0) {
      motion_compensate(ms, 1, motion_type, mx, my, have, second_field);
    }
  }

  void put_blocks(std::array<std::array<std::int32_t, 64>, 12>& blocks, int coded, int block_count, bool intra,
                  bool field_dct, int mx, int my) {
    Picture& cur = pictures_[static_cast<std::size_t>(current_)];
    const bool field_pic = picture_structure_ != kFramePicture;
    const int parity = picture_structure_ == kBottomField ? 1 : 0;
    for (int b = 0; b < block_count; ++b) {
      if (!intra && (coded & (1 << (block_count - 1 - b))) == 0) continue;
      const int p = b < 4 ? 0 : (b & 1) == 0 ? 1 : 2;
      int stride = cur.stride[p];
      std::uint8_t* base = cur.plane[p].data();
      if (field_pic) {
        base += parity * stride;
        stride *= 2;
      }
      int bx;
      int by;
      int block_stride = stride;
      if (p == 0) {
        bx = mx * 16 + (b & 1) * 8;
        if (field_dct) {
          by = my * 16 + (b >> 1);
          block_stride = stride * 2;
        } else {
          by = my * 16 + (b >> 1) * 8;
        }
      } else {
        const int k = (b - 4) >> 1;  // 0..3: chroma block of this component
        const int cw = chroma_w();
        const int ch = chroma_h();
        const int cbx = mx * cw;
        const int cby = my * ch;
        if (chroma_format_ == 1) {
          bx = cbx;
          by = cby;
        } else {
          // 4:2:2: k 0 top, 1 bottom. 4:4:4: k 0 TL, 1 BL, 2 TR, 3 BR.
          bx = cbx + (k >> 1) * 8;
          if (field_dct) {
            by = cby + (k & 1);
            block_stride = stride * 2;
          } else {
            by = cby + (k & 1) * 8;
          }
        }
      }
      std::uint8_t* dst = base + static_cast<std::ptrdiff_t>(by) * stride + bx;
      if (intra) {
        idct8x8_put(blocks[static_cast<std::size_t>(b)].data(), dst, block_stride);
      } else {
        idct8x8_add(blocks[static_cast<std::size_t>(b)].data(), dst, block_stride);
      }
    }
  }

  void decode_slice(std::size_t from, std::size_t to, bool second_field) {
    BitReader br(span(from + 4, to));
    const bool field_pic = picture_structure_ != kFramePicture;
    const int mb_rows = field_pic ? mb_height_ / 2 : mb_height_;
    int row = buffer_[from + 3] - 1;
    if (mpeg2_ && height_ > 2800) row += static_cast<int>(br.bits(3)) << 7;
    if (row >= mb_rows) return;
    int qcode = static_cast<int>(br.bits(5));
    if (mpeg2_) {
      if (br.peek(1) == 1) {
        br.skip(1 + 1 + 7);  // intra_slice_flag, intra_slice, reserved
        while (br.bit()) br.skip(8);
      } else {
        br.skip(1);
      }
    } else {
      while (br.bit()) br.skip(8);
    }
    if (qcode == 0) return;
    reset_dc();
    MotionState ms;
    int mb_addr = row * mb_width_ - 1;
    const int mb_count = mb_width_ * mb_rows;
    bool first = true;
    std::array<std::array<std::int32_t, 64>, 12> blocks{};
    const int block_count = chroma_format_ == 1 ? 6 : (chroma_format_ == 2 ? 8 : 12);
    while (true) {
      if (!first && br.peek(23) == 0) return;
      if (br.overrun() || br.bits_left() == 0) return;
      int increment = 0;
      while (true) {
        const int sym = tables_.mba.decode(br);
        if (sym == kMbaStuffing) {
          if (br.overrun()) return;
          continue;
        }
        if (sym == kMbaEscape) {
          increment += 33;
          if (increment > mb_count) return;
          continue;
        }
        if (sym == Vlc::invalid) return;
        increment += sym;
        break;
      }
      if (first) {
        mb_addr += increment;
        first = false;
      } else {
        // Skipped macroblocks.
        for (int k = 1; k < increment; ++k) {
          ++mb_addr;
          if (mb_addr >= mb_count) return;
          const int sx = mb_addr % mb_width_;
          const int sy = mb_addr / mb_width_;
          if (picture_type_ == kTypeP) {
            predict_skipped_p(sx, sy, second_field);
            ms = MotionState{};
          } else if (picture_type_ == kTypeB) {
            if ((ms.flags & kMbIntra) != 0) return;  // not allowed after an intra macroblock
            predict_inter(ms, ms.flags, ms.type, sx, sy, second_field);
          } else {
            return;  // I pictures cannot skip
          }
          reset_dc();
          mark_decoded(sx, sy, field_pic);
        }
        ++mb_addr;
      }
      if (mb_addr < 0 || mb_addr >= mb_count) return;
      const int mx = mb_addr % mb_width_;
      const int my = mb_addr / mb_width_;

      // macroblock_modes()
      int flags;
      switch (picture_type_) {
        case kTypeI: flags = tables_.mb_i.decode(br); break;
        case kTypeP: flags = tables_.mb_p.decode(br); break;
        case kTypeB: flags = tables_.mb_b.decode(br); break;
        default: flags = tables_.mb_d.decode(br); break;
      }
      if (flags == Vlc::invalid) return;
      const bool intra = (flags & kMbIntra) != 0;
      int motion_type = field_pic ? kMcField : kMcFrame;
      bool field_dct = false;
      if (mpeg2_) {
        if ((flags & (kMbForward | kMbBackward)) != 0) {
          if (field_pic) {
            motion_type = static_cast<int>(br.bits(2));
          } else if (!frame_pred_frame_dct_) {
            motion_type = static_cast<int>(br.bits(2));
          }
          if (motion_type == 0) return;
        }
        if (!field_pic && !frame_pred_frame_dct_ && (intra || (flags & kMbPattern) != 0)) field_dct = br.bit();
      }
      if ((flags & kMbQuant) != 0) {
        qcode = static_cast<int>(br.bits(5));
        if (qcode == 0) return;
      }
      const int qscale = quantiser_scale(qcode);
      if (motion_type == kMcDualPrime && picture_type_ != kTypeP) return;

      // Motion vectors.
      if ((flags & kMbForward) != 0 || (intra && concealment_mv_)) {
        const int type = intra ? (field_pic ? kMcField : kMcFrame) : motion_type;
        if (!motion_vectors(br, ms, 0, type)) return;
      }
      if ((flags & kMbBackward) != 0) {
        if (!motion_vectors(br, ms, 1, motion_type)) return;
      }
      if (intra && concealment_mv_) br.skip(1);  // marker

      // Coded block pattern.
      int cbp = 0;
      if ((flags & kMbPattern) != 0) {
        cbp = tables_.cbp.decode(br);
        if (cbp == Vlc::invalid) return;
        if (chroma_format_ == 2) cbp = (cbp << 2) | static_cast<int>(br.bits(2));
        if (chroma_format_ == 3) cbp = (cbp << 6) | static_cast<int>(br.bits(6));
      } else if (intra) {
        cbp = (1 << block_count) - 1;
      }

      // Blocks.
      if (!intra) reset_dc();
      for (int b = 0; b < block_count; ++b) {
        auto& blk = blocks[static_cast<std::size_t>(b)];
        if (!intra && (cbp & (1 << (block_count - 1 - b))) == 0) continue;
        blk.fill(0);
        if (!decode_block(br, b, intra, qscale, blk.data())) return;
      }
      if (picture_type_ == kTypeD) br.skip(1);  // end_of_macroblock
      if (br.overrun()) return;

      // Reconstruction.
      if (intra) {
        if (!concealment_mv_) ms = MotionState{};
        ms.flags = flags;
        put_blocks(blocks, cbp, block_count, true, field_dct, mx, my);
      } else {
        if (picture_type_ == kTypeP && (flags & kMbForward) == 0) {
          // No motion compensation: PMVs are reset.
          for (auto& r : ms.pmv) {
            for (auto& s : r) s[0] = s[1] = 0;
          }
          for (auto& r : ms.vector) {
            for (auto& s : r) s[0] = s[1] = 0;
          }
          motion_type = field_pic ? kMcField : kMcFrame;
          if (field_pic) ms.select[0][0] = picture_structure_ == kBottomField ? 1 : 0;
        }
        ms.flags = flags;
        ms.type = motion_type;
        predict_inter(ms, flags, motion_type, mx, my, second_field);
        put_blocks(blocks, cbp, block_count, false, field_dct, mx, my);
      }
      mark_decoded(mx, my, field_pic);
    }
  }

  void mark_decoded(int mx, int my, bool field_pic) {
    if (!field_pic) {
      decoded_[static_cast<std::size_t>(my * mb_width_ + mx)] = 1;
      return;
    }
    // A field macroblock covers half of two frame macroblocks.
    const int top = (my * 32) / 16;
    for (int r = top; r < top + 2 && r < mb_height_; ++r) {
      std::uint8_t& d = decoded_[static_cast<std::size_t>(r * mb_width_ + mx)];
      d = static_cast<std::uint8_t>(d | (picture_structure_ == kTopField ? 2 : 4));
      if (d == 6) d = 1;
    }
  }

  Tables tables_;
  CodecParams params_;

  // Stream assembly.
  std::vector<std::uint8_t> buffer_;
  std::size_t scan_ = 0;
  std::uint64_t base_ = 0;
  std::deque<PendingTimestamp> timestamps_;

  // Sequence.
  std::uint32_t base_width_ = 0;
  std::uint32_t base_height_ = 0;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  int mb_width_ = 0;
  int mb_height_ = 0;
  std::array<std::uint32_t, 4> allocated_layout_{};
  bool mpeg2_ = false;
  int chroma_format_ = 1;
  bool progressive_sequence_ = true;
  bool low_delay_ = false;
  int frame_rate_code_ = 3;
  int frame_rate_ext_n_ = 0;
  int frame_rate_ext_d_ = 0;
  int matrix_coefficients_ = 2;
  std::array<std::uint8_t, 64> intra_matrix_{};
  std::array<std::uint8_t, 64> inter_matrix_{};
  std::array<std::uint8_t, 64> chroma_intra_matrix_{};
  std::array<std::uint8_t, 64> chroma_inter_matrix_{};
  bool closed_gop_ = false;
  bool broken_link_ = false;
  bool drop_b_ = false;

  // Picture.
  bool picture_header_seen_ = false;
  int picture_type_ = 0;
  bool full_pel_[2] = {false, false};
  int f_code_[2][2] = {{1, 1}, {1, 1}};
  int intra_dc_precision_ = 0;
  int picture_structure_ = kFramePicture;
  bool top_field_first_ = true;
  bool frame_pred_frame_dct_ = true;
  bool concealment_mv_ = false;
  bool q_scale_type_ = false;
  bool intra_vlc_format_ = false;
  bool alternate_scan_ = false;
  bool repeat_first_field_ = false;
  bool progressive_frame_ = true;
  int dc_pred_[3] = {128, 128, 128};

  // Frame being decoded and references.
  std::array<Picture, 3> pictures_;
  int forward_ = -1;
  int backward_ = -1;
  int current_ = 0;
  int current_type_ = 0;
  std::optional<std::int64_t> current_pts_;
  std::int64_t current_duration_ = 0;
  bool current_rff_ = false;
  bool current_tff_ = true;
  bool first_field_pending_ = false;
  int first_field_structure_ = 0;
  std::vector<std::uint8_t> decoded_;

  // Output.
  std::optional<VideoFrame> held_;
  std::deque<VideoFrame> output_;
  bool have_last_pts_ = false;
  std::int64_t last_pts_ = 0;
  std::int64_t last_duration_ = 40;
};

}  // namespace

std::unique_ptr<codec::VideoDecoder> make_decoder(bool /*mpeg2*/) { return std::make_unique<Mpeg12Decoder>(); }

}  // namespace cvt::legacy::mpeg12
