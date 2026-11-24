// Convertible core - video family: elementary-stream helpers.
//
// Containers disagree on how a codec's bitstream is packaged: MP4, Matroska
// and FLV store H.264/HEVC as length-prefixed NAL units with the parameter
// sets in an avcC/hvcC record, MPEG-TS, MPEG-PS, AVI and raw files use
// Annex-B start codes with in-band parameter sets; AAC is raw with an
// AudioSpecificConfig in MP4/Matroska/FLV and ADTS-framed in TS and .aac.
// These helpers parse the headers the muxers need and convert between the
// forms, so that any container can carry any stream it can tag.
//
// Decoders registered in codec::Registry always receive the MP4 form:
//   h264/hevc  4-byte length-prefixed NAL units, extradata = avcC/hvcC payload
//   aac        raw access units, extradata = AudioSpecificConfig
//   av1        low-overhead OBUs (Section 5), extradata = av1C payload when known
//   opus       extradata = OpusHead (as in Ogg/Matroska)
//   vorbis/theora  extradata = Xiph-laced identification/comment/setup headers
//   flac       extradata = "fLaC" + metadata blocks
//   rawvideo   extradata = 4-byte layout tag (see builtin_codecs.hpp)
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::video::bits {

// --------------------------------------------------------------------------- bit reader

/// MSB-first bit reader with Exp-Golomb codes; reads past the end yield 0
/// and latch `failed()`.
class BitReader {
 public:
  explicit BitReader(ByteView data) noexcept : data_(data) {}
  std::uint32_t bits(int count) noexcept;
  bool flag() noexcept { return bits(1) != 0; }
  std::uint32_t ue() noexcept;
  std::int32_t se() noexcept;
  void skip(std::uint64_t count) noexcept;
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] std::uint64_t position() const noexcept { return pos_; }
  [[nodiscard]] std::uint64_t remaining() const noexcept { return data_.size() * 8 > pos_ ? data_.size() * 8 - pos_ : 0; }
  void align() noexcept { pos_ = (pos_ + 7) & ~std::uint64_t{7}; }

 private:
  ByteView data_;
  std::uint64_t pos_ = 0;
  bool failed_ = false;
};

/// MSB-first bit writer.
class BitWriter {
 public:
  void put(std::uint32_t value, int count);
  void flag(bool value) { put(value ? 1 : 0, 1); }
  void align_zero();
  [[nodiscard]] Bytes take();

 private:
  Bytes out_;
  std::uint32_t acc_ = 0;
  int used_ = 0;
};

/// Remove H.264/HEVC emulation-prevention bytes (00 00 03 -> 00 00).
[[nodiscard]] Bytes unescape_rbsp(ByteView nal);

// --------------------------------------------------------------------------- NAL units

/// True when the data begins with an Annex-B start code (00 00 01 or 00 00 00 01).
[[nodiscard]] bool starts_with_start_code(ByteView data) noexcept;
/// The NAL units of an Annex-B byte stream (start codes and trailing zero bytes removed).
[[nodiscard]] std::vector<ByteView> split_annexb(ByteView data);
/// The NAL units of a length-prefixed buffer; nullopt when a length overruns.
[[nodiscard]] std::optional<std::vector<ByteView>> split_length_prefixed(ByteView data, int length_size);
/// Re-frame NAL units with 4-byte big-endian lengths.
[[nodiscard]] Bytes to_length_prefixed(const std::vector<ByteView>& nals);
/// Re-frame NAL units with 4-byte start codes.
[[nodiscard]] Bytes to_annexb(const std::vector<ByteView>& nals);

[[nodiscard]] inline int h264_nal_type(ByteView nal) noexcept {
  return nal.empty() ? -1 : std::to_integer<int>(nal[0]) & 0x1F;
}
[[nodiscard]] inline int hevc_nal_type(ByteView nal) noexcept {
  return nal.empty() ? -1 : (std::to_integer<int>(nal[0]) >> 1) & 0x3F;
}

/// Parameter-set NAL units of an H.264 or HEVC stream, in avcC/hvcC order.
struct ParameterSets {
  std::vector<Bytes> vps;  ///< HEVC only
  std::vector<Bytes> sps;
  std::vector<Bytes> pps;
  int length_size = 4;
  [[nodiscard]] bool complete(bool hevc) const noexcept { return !sps.empty() && !pps.empty() && (!hevc || !vps.empty()); }
};

/// Parse an avcC (AVCDecoderConfigurationRecord) payload.
[[nodiscard]] std::optional<ParameterSets> parse_avcc(ByteView data);
/// Parse an hvcC (HEVCDecoderConfigurationRecord) payload.
[[nodiscard]] std::optional<ParameterSets> parse_hvcc(ByteView data);
/// Build an avcC payload (4-byte lengths); needs at least one SPS.
[[nodiscard]] Result<Bytes> build_avcc(const ParameterSets& sets);
/// Build an hvcC payload (4-byte lengths); needs VPS, SPS and PPS.
[[nodiscard]] Result<Bytes> build_hvcc(const ParameterSets& sets);
/// Collect parameter sets found in-band in one access unit (deduplicated into `sets`).
void collect_parameter_sets(const std::vector<ByteView>& nals, bool hevc, ParameterSets& sets);

/// H.264 picture order counts of a sequence of access units (decode order),
/// for containers that store no presentation times (raw streams, AVI).
/// Supports pic_order_cnt_type 0 and 2; returns nullopt otherwise or when
/// the parameter sets are missing. Each entry is {POC, is IDR}.
[[nodiscard]] std::optional<std::vector<std::pair<std::int64_t, bool>>> h264_picture_order(
    const std::vector<std::vector<ByteView>>& access_units);

/// The same, one access unit at a time (keeps the decoder-side state).
class H264PocTracker {
 public:
  H264PocTracker();
  ~H264PocTracker();
  H264PocTracker(const H264PocTracker&) = delete;
  H264PocTracker& operator=(const H264PocTracker&) = delete;
  [[nodiscard]] std::optional<std::pair<std::int64_t, bool>> next(const std::vector<ByteView>& access_unit);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

/// Facts from a sequence parameter set.
struct SpsInfo {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  int profile = 0;
  int level = 0;
  int chroma_format = 1;
  int bit_depth_luma = 8;
  int bit_depth_chroma = 8;
  /// VUI timing: frames per second = fps_num / fps_den; both 0 when absent.
  std::uint32_t fps_num = 0;
  std::uint32_t fps_den = 0;
  /// VUI sample aspect ratio; 0:0 when unspecified.
  std::uint32_t sar_num = 0;
  std::uint32_t sar_den = 0;
  /// Pictures that may precede another one in decode order but follow it in
  /// output order (HEVC sps_max_num_reorder_pics; H.264 VUI max_num_reorder_frames
  /// when stated); nullopt when the stream does not say.
  std::optional<std::uint32_t> max_num_reorder;
  /// True for H.264 streams coded as frames only (no field pictures).
  bool frame_mbs_only = true;
};
[[nodiscard]] std::optional<SpsInfo> parse_h264_sps(ByteView nal);
[[nodiscard]] std::optional<SpsInfo> parse_hevc_sps(ByteView nal);

/// HEVC picture order counts (H.265 8.3.1), one access unit at a time, so
/// raw streams get presentation times that follow the B-frame reordering.
class HevcPocTracker {
 public:
  HevcPocTracker();
  ~HevcPocTracker();
  HevcPocTracker(const HevcPocTracker&) = delete;
  HevcPocTracker& operator=(const HevcPocTracker&) = delete;
  /// {POC, starts a coded video sequence} of an access unit (decode order);
  /// nullopt when its parameter sets are unknown or the slice header is damaged.
  [[nodiscard]] std::optional<std::pair<std::int64_t, bool>> next(const std::vector<ByteView>& access_unit);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

/// True when an access unit contains an IDR/IRAP picture.
[[nodiscard]] bool h264_is_keyframe(const std::vector<ByteView>& nals) noexcept;
[[nodiscard]] bool hevc_is_keyframe(const std::vector<ByteView>& nals) noexcept;

// --------------------------------------------------------------------------- AAC

struct AacConfig {
  int object_type = 2;  ///< 2 = AAC LC
  int sampling_index = 4;
  std::uint32_t sample_rate = 44100;
  int channel_config = 2;
  int samples_per_frame = 1024;
};
struct AdtsHeader {
  AacConfig config;
  std::size_t header_size = 7;
  std::size_t frame_size = 0;  ///< header included
};
[[nodiscard]] std::optional<AdtsHeader> parse_adts(ByteView data) noexcept;
[[nodiscard]] std::optional<AacConfig> parse_asc(ByteView data) noexcept;
[[nodiscard]] Bytes build_asc(const AacConfig& config);
[[nodiscard]] Bytes build_adts_header(const AacConfig& config, std::size_t payload_size);
[[nodiscard]] int aac_sampling_index(std::uint32_t rate) noexcept;
[[nodiscard]] std::uint32_t aac_sample_rate(int index) noexcept;

// --------------------------------------------------------------------------- MPEG audio

struct MpegAudioHeader {
  int version = 1;  ///< 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5
  int layer = 3;
  std::uint32_t bitrate_kbps = 0;
  std::uint32_t sample_rate = 0;
  int channels = 2;
  std::size_t frame_size = 0;
  int samples_per_frame = 1152;
};
[[nodiscard]] std::optional<MpegAudioHeader> parse_mpeg_audio(ByteView data) noexcept;

// --------------------------------------------------------------------------- other codecs

/// av1C payload from a sequence header OBU found in `obus` (low-overhead form).
[[nodiscard]] std::optional<Bytes> build_av1c(ByteView obus);
/// Width/height of an AV1 sequence header in `obus`.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> av1_dimensions(ByteView obus);
/// Strip temporal-delimiter OBUs (MP4/Matroska store AV1 without them).
[[nodiscard]] Bytes av1_strip_temporal_delimiters(ByteView obus);
/// True when the temporal unit starts a key frame (sequence header + key frame).
[[nodiscard]] bool av1_is_keyframe(ByteView obus) noexcept;

struct Vp9Info {
  int profile = 0;
  int bit_depth = 8;
  int chroma_subsampling = 1;  ///< vpcC value: 1 = 4:2:0 colocated
  bool full_range = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool keyframe = false;
};
[[nodiscard]] std::optional<Vp9Info> parse_vp9_frame(ByteView frame) noexcept;
/// Key-frame flag and size of a VP8 frame.
[[nodiscard]] std::optional<Vp9Info> parse_vp8_frame(ByteView frame) noexcept;
/// vpcC payload (version 1) for VP8/VP9.
[[nodiscard]] Bytes build_vpcc(const Vp9Info& info);

/// dOps payload from an OpusHead packet and back.
[[nodiscard]] std::optional<Bytes> opus_head_to_dops(ByteView head);
[[nodiscard]] std::optional<Bytes> dops_to_opus_head(ByteView dops);
/// A minimal OpusHead for `channels` (mapping family 0/1) and pre-skip.
[[nodiscard]] Bytes make_opus_head(int channels, std::uint32_t input_rate, std::uint16_t pre_skip);
/// Pre-skip field of an OpusHead (0 when malformed).
[[nodiscard]] std::uint16_t opus_pre_skip(ByteView head) noexcept;
/// Samples (at 48 kHz) in one Opus packet, 0 when malformed.
[[nodiscard]] int opus_packet_samples(ByteView packet) noexcept;

/// FLAC: STREAMINFO facts from "fLaC"+blocks, or from bare blocks.
struct FlacInfo {
  std::uint32_t sample_rate = 0;
  int channels = 0;
  int bits = 0;
  std::uint64_t total_samples = 0;
  int max_block = 0;
};
[[nodiscard]] std::optional<FlacInfo> parse_flac_streaminfo(ByteView data) noexcept;

/// Xiph lacing as used in Matroska CodecPrivate for Vorbis/Theora.
[[nodiscard]] std::optional<std::vector<Bytes>> split_xiph_lacing(ByteView data);
[[nodiscard]] Bytes join_xiph_lacing(const std::vector<Bytes>& packets);

/// Vorbis identification header facts.
struct VorbisInfo {
  int channels = 0;
  std::uint32_t sample_rate = 0;
  int blocksize0 = 256;
  int blocksize1 = 2048;
};
[[nodiscard]] std::optional<VorbisInfo> parse_vorbis_id(ByteView packet) noexcept;
/// Vorbis packet durations: the mode table of the setup header, found by
/// scanning it backwards (the codebooks in front of it need not be parsed).
class VorbisDurations {
 public:
  /// Needs the identification and setup headers; false when they are unusable.
  bool init(ByteView identification, ByteView setup);
  /// Samples produced by an audio packet (0 for the first one); -1 when malformed.
  int next(ByteView packet);
  [[nodiscard]] bool ready() const noexcept { return modes_ > 0; }

 private:
  std::vector<bool> blockflag_;
  int modes_ = 0;
  int mode_bits_ = 0;
  int bs0_ = 256;
  int bs1_ = 2048;
  int previous_ = 0;
};

/// Theora identification header facts (frame size, frame rate).
struct TheoraInfo {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t fps_num = 0;
  std::uint32_t fps_den = 0;
  int keyframe_shift = 6;
};
[[nodiscard]] std::optional<TheoraInfo> parse_theora_id(ByteView packet) noexcept;

/// MPEG-1/2 video sequence header facts.
struct Mpeg12Info {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int64_t fps_num = 0;
  std::int64_t fps_den = 1;
  bool mpeg2 = false;
};
[[nodiscard]] std::optional<Mpeg12Info> parse_mpeg12_sequence(ByteView data) noexcept;
/// MPEG-4 Part 2: the configuration (VOS/VO/VOL headers) before the first VOP, or empty.
[[nodiscard]] Bytes mpeg4_config(ByteView frame);

/// Big-endian FourCC from four characters.
[[nodiscard]] constexpr std::uint32_t fourcc(std::string_view s) noexcept {
  return s.size() < 4 ? 0
                      : (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
                            (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
                            (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8) |
                            static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}
/// Printable FourCC (non-printable characters as '?').
[[nodiscard]] std::string fourcc_text(std::uint32_t value);

}  // namespace cvt::video::bits
