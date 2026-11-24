// Convertible core - codec_legacy family: Theora, through the imported
// Xiph libtheora 1.1.1 (BSD-3-Clause; see core/src/imported/libtheora).
//
// Packet interface without Ogg: the three header packets (identification,
// comment, setup) travel as extradata, either Xiph-laced (Matroska/WebM
// CodecPrivate: 0x02, lacing sizes, packets) or as 16-bit big-endian
// length-prefixed packets; they may also arrive in band as the first
// packets (Ogg demuxers), recognised by the 0x80..0x82 packet type byte.
// A zero-length data packet repeats the previous picture. The encoder's
// extradata is Xiph-laced. Output: I420/I422/I444 cropped to the picture
// region, BT.601 (Theora's Rec. 470 colour spaces), limited range.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

#include "util/bytes.hpp"
#include "codec/ids.hpp"
#include "families/codec_legacy/codecs.hpp"
#include "families/codec_legacy/common/codec_base.hpp"

#include <theora/theoradec.h>
#include <theora/theoraenc.h>

namespace cvt::legacy {

namespace {

/// Splits Theora extradata into header packets (empty on failure).
std::vector<std::vector<std::uint8_t>> split_headers(std::span<const std::uint8_t> d) {
  std::vector<std::vector<std::uint8_t>> out;
  if (d.size() >= 3 && d[0] == 2) {
    // Xiph lacing.
    std::size_t at = 1;
    std::size_t sizes[2] = {0, 0};
    for (std::size_t& size : sizes) {
      while (at < d.size() && d[at] == 255) {
        size += 255;
        ++at;
      }
      if (at >= d.size()) return {};
      size += d[at++];
    }
    if (sizes[0] + sizes[1] > d.size() - at) return {};
    const std::size_t third = d.size() - at - sizes[0] - sizes[1];
    out.emplace_back(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + sizes[0]));
    at += sizes[0];
    out.emplace_back(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + sizes[1]));
    at += sizes[1];
    out.emplace_back(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + third));
    return out;
  }
  // 16-bit length prefixes.
  std::size_t at = 0;
  while (at + 2 <= d.size() && out.size() < 3) {
    const std::size_t n = static_cast<std::size_t>(d[at]) << 8 | d[at + 1];
    at += 2;
    if (n > d.size() - at) return {};
    out.emplace_back(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + n));
    at += n;
  }
  if (out.size() != 3) return {};
  return out;
}

ogg_packet make_packet(const std::uint8_t* data, std::size_t size, std::int64_t number, bool bos) {
  ogg_packet op{};
  op.packet = const_cast<unsigned char*>(data);
  op.bytes = static_cast<long>(size);
  op.b_o_s = bos ? 1 : 0;
  op.e_o_s = 0;
  op.granulepos = -1;
  op.packetno = number;
  return op;
}

class TheoraDecoder final : public codec::VideoDecoder {
 public:
  TheoraDecoder() {
    th_info_init(&info_);
    th_comment_init(&comment_);
  }
  ~TheoraDecoder() override {
    if (decoder_ != nullptr) th_decode_free(decoder_);
    if (setup_ != nullptr) th_setup_free(setup_);
    th_comment_clear(&comment_);
    th_info_clear(&info_);
  }
  TheoraDecoder(const TheoraDecoder&) = delete;
  TheoraDecoder& operator=(const TheoraDecoder&) = delete;

  Result<void> configure(const CodecParams& params) override {
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 1000;
    if (!params.extradata.empty()) {
      const auto headers = split_headers(as_u8(ByteView(params.extradata)));
      if (headers.empty()) return fail(Status::Decode, "malformed Theora header packets in extradata");
      for (const auto& h : headers) CVT_TRY(header(h.data(), h.size()));
    }
    return {};
  }

  Result<void> send_packet(const Packet& packet) override {
    const auto* data = reinterpret_cast<const std::uint8_t*>(packet.bytes.data());
    const std::size_t size = packet.bytes.size();
    if (decoder_ == nullptr) {
      if (size > 0 && (data[0] & 0x80) != 0) return header(data, size);
      return fail(Status::Decode, "Theora data packet before the header packets");
    }
    if (size > 0 && (data[0] & 0x80) != 0) return {};  // repeated headers
    // Inter frames before the first key frame have nothing to refer to: drop them.
    if (size > 0 && (data[0] & 0x40) == 0) have_keyframe_ = true;
    if (!have_keyframe_) return {};
    ogg_packet op = make_packet(data, size, ++packet_number_, false);
    ogg_int64_t granule = 0;
    const int r = th_decode_packetin(decoder_, &op, &granule);
    if (r < 0) return fail(Status::Decode, "damaged Theora packet ({})", r);
    VideoFrame frame;
    if (r == TH_DUPFRAME && last_) {
      frame = *last_;
    } else {
      th_ycbcr_buffer buffer;
      if (th_decode_ycbcr_out(decoder_, buffer) != 0) return fail(Status::Decode, "Theora decoder produced no picture");
      frame = convert(buffer);
    }
    frame.pts_ms = ticks_to_ms(packet.pts, params_.timescale);
    frame.duration_ms = packet.duration > 0 ? ticks_to_ms(packet.duration, params_.timescale) : frame_duration_;
    last_ = frame;
    output_.push_back(std::move(frame));
    return {};
  }

  Result<std::optional<VideoFrame>> receive_frame() override {
    if (output_.empty()) return std::optional<VideoFrame>{};
    VideoFrame f = std::move(output_.front());
    output_.pop_front();
    return std::optional<VideoFrame>(std::move(f));
  }
  Result<void> flush() override { return {}; }

 private:
  Result<void> header(const std::uint8_t* data, std::size_t size) {
    if (decoder_ != nullptr) return {};
    ogg_packet op = make_packet(data, size, header_count_, header_count_ == 0);
    const int r = th_decode_headerin(&info_, &comment_, &setup_, &op);
    if (r < 0) return fail(Status::Decode, "invalid Theora header packet");
    ++header_count_;
    if (header_count_ == 3) {
      // All three headers seen: the first data packet follows.
      CVT_TRY(check_dimensions(info_.pic_width, info_.pic_height));
      if (info_.pic_x + info_.pic_width > info_.frame_width || info_.pic_y + info_.pic_height > info_.frame_height) {
        return fail(Status::Decode, "Theora picture region outside the frame");
      }
      if (info_.pixel_fmt == TH_PF_RSVD) return fail(Status::Decode, "reserved Theora pixel format");
      decoder_ = th_decode_alloc(&info_, setup_);
      if (decoder_ == nullptr) return fail(Status::Decode, "unsupported Theora stream");
      if (info_.fps_numerator > 0 && info_.fps_denominator > 0) {
        frame_duration_ = std::max<std::int64_t>(1, 1000LL * info_.fps_denominator / info_.fps_numerator);
      }
    }
    return {};
  }

  VideoFrame convert(const th_ycbcr_buffer buffer) const {
    PixelFormat format = PixelFormat::I420;
    if (info_.pixel_fmt == TH_PF_422) format = PixelFormat::I422;
    if (info_.pixel_fmt == TH_PF_444) format = PixelFormat::I444;
    VideoFrame f = VideoFrame::allocate(format, info_.pic_width, info_.pic_height);
    for (std::size_t p = 0; p < 3; ++p) {
      const int sx = p == 0 ? 0 : ir::chroma_shift_x(format);
      const int sy = p == 0 ? 0 : ir::chroma_shift_y(format);
      const std::uint32_t x0 = info_.pic_x >> sx;
      const std::uint32_t y0 = info_.pic_y >> sy;
      const std::size_t row_bytes = ir::plane_row_bytes(format, p, info_.pic_width);
      const std::size_t rows = ir::plane_height(format, p, info_.pic_height);
      for (std::size_t y = 0; y < rows; ++y) {
        const unsigned char* src = buffer[p].data + static_cast<std::ptrdiff_t>(y0 + y) * buffer[p].stride + x0;
        ::cvt::util::copy_bytes(f.planes[p].data.data() + y * f.planes[p].stride, src, row_bytes);
      }
    }
    f.color = {ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Center};
    return f;
  }

  CodecParams params_;
  th_info info_{};
  th_comment comment_{};
  th_setup_info* setup_ = nullptr;
  th_dec_ctx* decoder_ = nullptr;
  int header_count_ = 0;
  std::int64_t packet_number_ = 2;
  bool have_keyframe_ = false;
  std::int64_t frame_duration_ = 40;
  std::optional<VideoFrame> last_;
  std::deque<VideoFrame> output_;
};

/// Theora encoder: 4:2:0, quality 1..100 mapped onto libtheora's 0..63
/// scale (90 -> 57), or a target bitrate; `preset` sets the speed level.
class TheoraEncoder final : public SimpleEncoder {
 public:
  ~TheoraEncoder() override {
    if (encoder_ != nullptr) th_encode_free(encoder_);
  }
  TheoraEncoder() = default;
  TheoraEncoder(const TheoraEncoder&) = delete;
  TheoraEncoder& operator=(const TheoraEncoder&) = delete;

 protected:
  Result<void> on_configure() override {
    if (params_.width > 1048560 || params_.height > 1048560) return fail(Status::InvalidArgument, "picture too large for Theora");
    th_info info;
    th_info_init(&info);
    info.frame_width = (params_.width + 15) & ~15u;
    info.frame_height = (params_.height + 15) & ~15u;
    info.pic_width = params_.width;
    info.pic_height = params_.height;
    info.pic_x = 0;
    info.pic_y = 0;
    info.colorspace = TH_CS_UNSPECIFIED;
    info.pixel_fmt = TH_PF_420;
    std::int64_t num = params_.fps.num > 0 && params_.fps.den > 0 ? params_.fps.num : 25;
    std::int64_t den = params_.fps.num > 0 && params_.fps.den > 0 ? params_.fps.den : 1;
    while (num > 0xFFFFFFFF || den > 0xFFFFFFFF) {
      num /= 2;
      den = std::max<std::int64_t>(1, den / 2);
    }
    info.fps_numerator = static_cast<ogg_uint32_t>(num);
    info.fps_denominator = static_cast<ogg_uint32_t>(den);
    info.aspect_numerator = 1;
    info.aspect_denominator = 1;
    if (params_.bitrate_kbps) {
      info.target_bitrate = static_cast<int>(std::min<std::uint32_t>(*params_.bitrate_kbps, 2000000) * 1000);
      info.quality = 0;
    } else {
      info.target_bitrate = 0;
      info.quality = std::clamp(static_cast<int>(std::lround(params_.quality * 63.0 / 100.0)), 0, 63);
    }
    const std::uint32_t keyint = params_.keyframe_interval > 0 ? params_.keyframe_interval : 64;
    int shift = 0;
    while ((1u << shift) < keyint && shift < 31) ++shift;
    info.keyframe_granule_shift = shift;
    encoder_ = th_encode_alloc(&info);
    th_info_clear(&info);
    if (encoder_ == nullptr) return fail(Status::InvalidArgument, "Theora encoder rejected the parameters");
    ogg_uint32_t frequency = keyint;
    th_encode_ctl(encoder_, TH_ENCCTL_SET_KEYFRAME_FREQUENCY_FORCE, &frequency, sizeof(frequency));
    int max_speed = 0;
    th_encode_ctl(encoder_, TH_ENCCTL_GET_SPLEVEL_MAX, &max_speed, sizeof(max_speed));
    int speed = params_.preset == EncodePreset::Fast ? max_speed : (params_.preset == EncodePreset::Small ? 0 : std::min(1, max_speed));
    th_encode_ctl(encoder_, TH_ENCCTL_SET_SPLEVEL, &speed, sizeof(speed));
    // Header packets -> Xiph-laced extradata.
    th_comment comment;
    th_comment_init(&comment);
    std::vector<std::vector<std::uint8_t>> headers;
    ogg_packet op;
    while (th_encode_flushheader(encoder_, &comment, &op) > 0) {
      headers.emplace_back(op.packet, op.packet + op.bytes);
    }
    th_comment_clear(&comment);
    if (headers.size() != 3) return fail(Status::Internal, "Theora encoder did not produce its headers");
    extradata_.push_back(2);
    for (std::size_t k = 0; k < 2; ++k) {
      std::size_t n = headers[k].size();
      while (n >= 255) {
        extradata_.push_back(255);
        n -= 255;
      }
      extradata_.push_back(static_cast<std::uint8_t>(n));
    }
    for (const auto& h : headers) extradata_.insert(extradata_.end(), h.begin(), h.end());
    frame_w_ = (params_.width + 15) & ~15u;
    frame_h_ = (params_.height + 15) & ~15u;
    for (std::size_t p = 0; p < 3; ++p) {
      const std::uint32_t pw = p == 0 ? frame_w_ : frame_w_ / 2;
      const std::uint32_t ph = p == 0 ? frame_h_ : frame_h_ / 2;
      planes_[p].assign(static_cast<std::size_t>(pw) * ph, 0);
    }
    return {};
  }

  Bytes extradata() const override { return to_bytes(std::span<const std::uint8_t>(extradata_)); }

  Result<Bytes> encode(const VideoFrame& frame, bool& keyframe) override {
    ColorInfo color{ir::ColorMatrix::BT601, ir::ColorRange::Limited, ir::ChromaSiting::Center};
    CVT_TRY_ASSIGN(const VideoFrame f, to_format(frame, PixelFormat::I420, color));
    th_ycbcr_buffer buffer;
    for (std::size_t p = 0; p < 3; ++p) {
      const std::uint32_t pw = p == 0 ? frame_w_ : frame_w_ / 2;
      const std::uint32_t ph = p == 0 ? frame_h_ : frame_h_ / 2;
      const std::uint32_t sw = ir::plane_width(PixelFormat::I420, p, params_.width);
      const std::uint32_t sh = ir::plane_height(PixelFormat::I420, p, params_.height);
      for (std::uint32_t y = 0; y < ph; ++y) {
        const std::uint8_t* src = f.planes[p].data.data() + static_cast<std::size_t>(std::min(y, sh - 1)) * f.planes[p].stride;
        std::uint8_t* dst = planes_[p].data() + static_cast<std::size_t>(y) * pw;
        ::cvt::util::copy_bytes(dst, src, sw);
        std::fill(dst + sw, dst + pw, src[sw - 1]);  // edge replication into the padding
      }
      buffer[p].width = static_cast<int>(pw);
      buffer[p].height = static_cast<int>(ph);
      buffer[p].stride = static_cast<int>(pw);
      buffer[p].data = planes_[p].data();
    }
    if (th_encode_ycbcr_in(encoder_, buffer) != 0) return fail(Status::Encode, "Theora encoder refused the picture");
    ogg_packet op;
    Bytes out;
    bool got = false;
    while (th_encode_packetout(encoder_, 0, &op) > 0) {
      out = to_bytes(std::span<const std::uint8_t>(op.packet, static_cast<std::size_t>(op.bytes)));
      keyframe = th_packet_iskeyframe(&op) == 1;
      got = true;
    }
    if (!got) return fail(Status::Encode, "Theora encoder produced no packet");
    return out;
  }

 private:
  th_enc_ctx* encoder_ = nullptr;
  std::vector<std::uint8_t> extradata_;
  std::uint32_t frame_w_ = 0;
  std::uint32_t frame_h_ = 0;
  std::array<std::vector<std::uint8_t>, 3> planes_;
};

}  // namespace

void add_theora_codecs(std::vector<CodecEntry>& out) {
  out.push_back({codec::id::theora, [] { return std::make_unique<TheoraDecoder>(); }, [] { return std::make_unique<TheoraEncoder>(); }});
}

}  // namespace cvt::legacy
