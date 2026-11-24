// Convertible core - codec_h264 family: H.264 bitstream helpers.
//
// NAL unit framing (Annex B byte streams and length-prefixed "AVCC"
// samples), sequence parameter set parsing and the `avcC` decoder
// configuration record (ISO/IEC 14496-15). Written in-house against ITU-T
// H.264; independent of the imported decoder and encoder.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/errors.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::h264 {

/// NAL unit types used by the wrappers (ITU-T H.264 table 7-1).
enum NalType : std::uint8_t {
  nal_slice = 1,
  nal_slice_idr = 5,
  nal_sei = 6,
  nal_sps = 7,
  nal_pps = 8,
  nal_aud = 9,
  nal_end_of_sequence = 10,
  nal_end_of_stream = 11,
  nal_filler = 12,
  nal_sps_extension = 13,
  nal_prefix = 14,
  nal_subset_sps = 15,
  nal_slice_extension = 20,
};

/// Type of a NAL unit (the low five bits of its first byte); 0 when empty.
[[nodiscard]] inline std::uint8_t nal_type(ByteView nal) noexcept {
  return nal.empty() ? 0 : static_cast<std::uint8_t>(std::to_integer<unsigned>(nal[0]) & 0x1Fu);
}
/// True for coded slices of the primary picture (types 1..5).
[[nodiscard]] inline bool is_vcl(std::uint8_t type) noexcept { return type >= 1 && type <= 5; }

/// True when `data` starts with an Annex B start code (00 00 01 or 00 00 00 01).
[[nodiscard]] bool starts_with_start_code(ByteView data) noexcept;

/// The NAL units of an Annex B byte stream, as views into `stream` (start
/// codes and trailing zero bytes removed, empty units skipped). Bytes before
/// the first start code are ignored.
[[nodiscard]] std::vector<ByteView> split_annexb(ByteView stream);

/// The access units of an Annex B byte stream: each view runs from the start
/// code of the unit's first NAL to the start of the next access unit. A new
/// unit starts at an access unit delimiter, at SEI / parameter sets / prefix
/// NAL units that follow a coded slice, and at a slice with first_mb_in_slice
/// 0 after a coded slice (so each field of a field pair is its own unit).
[[nodiscard]] std::vector<ByteView> split_access_units(ByteView stream);

/// The NAL units of a length-prefixed sample (`length_size` = 1, 2 or 4
/// bytes, big-endian). Fails with Status::Decode when a length runs past the
/// end of the data.
[[nodiscard]] Result<std::vector<ByteView>> split_length_prefixed(ByteView sample, int length_size);

/// Annex B byte stream (4-byte start codes) from NAL units.
[[nodiscard]] Bytes to_annexb(std::span<const ByteView> nals);
/// Length-prefixed sample from NAL units. Fails with Status::Encode when a
/// unit is too long for `length_size`.
[[nodiscard]] Result<Bytes> to_length_prefixed(std::span<const ByteView> nals, int length_size = 4);

/// Convert one sample between the two framings.
[[nodiscard]] Result<Bytes> annexb_to_length_prefixed(ByteView stream, int length_size = 4);
[[nodiscard]] Result<Bytes> length_prefixed_to_annexb(ByteView sample, int length_size = 4);

/// The RBSP of a NAL unit: the header byte dropped and emulation prevention
/// bytes (00 00 03) removed.
[[nodiscard]] Bytes nal_to_rbsp(ByteView nal);

/// Exp-Golomb bit reader over an RBSP. Reads past the end yield zero bits
/// and set `overrun()`, so parsers check once at the end.
class BitReader {
 public:
  explicit BitReader(ByteView data) noexcept : data_(data) {}
  [[nodiscard]] std::uint32_t bits(int count) noexcept;  ///< 0..32 bits
  [[nodiscard]] bool flag() noexcept { return bits(1) != 0; }
  [[nodiscard]] std::uint32_t ue() noexcept;  ///< unsigned Exp-Golomb (values up to 2^32 - 2)
  [[nodiscard]] std::int32_t se() noexcept;   ///< signed Exp-Golomb
  void skip(std::uint64_t count) noexcept;
  [[nodiscard]] bool overrun() const noexcept { return overrun_; }
  [[nodiscard]] std::uint64_t position() const noexcept { return position_; }
  [[nodiscard]] std::uint64_t size_bits() const noexcept { return static_cast<std::uint64_t>(data_.size()) * 8; }

 private:
  ByteView data_;
  std::uint64_t position_ = 0;
  bool overrun_ = false;
};

/// The fields of a sequence parameter set that the wrappers need.
struct Sps {
  std::uint8_t profile_idc = 0;
  std::uint8_t constraint_flags = 0;  ///< constraint_set0..5 flags, MSB first
  std::uint8_t level_idc = 0;
  std::uint32_t id = 0;
  std::uint32_t chroma_format_idc = 1;  ///< 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
  bool separate_colour_plane = false;
  std::uint32_t bit_depth_luma = 8;
  std::uint32_t bit_depth_chroma = 8;
  bool lossless_bypass = false;  ///< qpprime_y_zero_transform_bypass_flag
  std::uint32_t max_num_ref_frames = 0;
  bool frame_mbs_only = true;
  bool mb_adaptive_frame_field = false;
  /// Coded size in luma samples (whole macroblocks).
  std::uint32_t coded_width = 0;
  std::uint32_t coded_height = 0;
  /// Cropping rectangle, in luma samples.
  std::uint32_t crop_left = 0;
  std::uint32_t crop_right = 0;
  std::uint32_t crop_top = 0;
  std::uint32_t crop_bottom = 0;
  /// Displayed size (coded size minus cropping).
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  // -- VUI (defaults when absent) --------------------------------------------
  bool vui_present = false;
  /// Sample aspect ratio; 0:0 when unknown.
  std::uint32_t sar_num = 0;
  std::uint32_t sar_den = 0;
  bool video_full_range = false;
  bool colour_description_present = false;
  std::uint8_t colour_primaries = 2;  ///< ITU-T H.273 codes, 2 = unspecified
  std::uint8_t transfer_characteristics = 2;
  std::uint8_t matrix_coefficients = 2;
  bool chroma_loc_present = false;
  std::uint32_t chroma_sample_loc_type = 0;
  bool timing_info_present = false;
  std::uint32_t num_units_in_tick = 0;
  std::uint32_t time_scale = 0;
  bool fixed_frame_rate = false;
  bool bitstream_restriction = false;
  std::uint32_t max_num_reorder_frames = 0;
  std::uint32_t max_dec_frame_buffering = 0;
  /// True when the VUI was cut short or malformed; the fields above then
  /// keep the values read before the problem.
  bool vui_damaged = false;

  /// Frame rate from the VUI timing information (time_scale / (2 *
  /// num_units_in_tick)), reduced; 0/1 when absent.
  [[nodiscard]] ir::Rational frame_rate() const noexcept;
  /// Colour interpretation for decoded frames (matrix, range, chroma siting),
  /// with the usual guesses for unspecified values (BT.709 from 720 lines up,
  /// BT.601 below).
  [[nodiscard]] ir::ColorInfo color_info() const noexcept;
  /// Human-readable profile name ("Constrained Baseline", "High 4:2:2", ...).
  [[nodiscard]] std::string profile_name() const;
};

/// Parse a sequence parameter set NAL unit (header byte included).
[[nodiscard]] Result<Sps> parse_sps(ByteView nal);

/// Why the imported decoder cannot decode a stream with this SPS, as a
/// message for end users; nullopt when it can.
[[nodiscard]] std::optional<std::string> unsupported_reason(const Sps& sps);

/// The `avcC` decoder configuration record (ISO/IEC 14496-15 5.3.3.1).
struct AvcConfig {
  std::uint8_t profile_idc = 0;
  std::uint8_t profile_compatibility = 0;
  std::uint8_t level_idc = 0;
  int length_size = 4;  ///< bytes per NAL length prefix in samples: 1, 2 or 4
  std::vector<Bytes> sps;
  std::vector<Bytes> pps;
  std::vector<Bytes> sps_ext;
  /// Present for the High profiles when the record carries the extension.
  std::optional<std::uint32_t> chroma_format_idc;
  std::optional<std::uint32_t> bit_depth_luma;
  std::optional<std::uint32_t> bit_depth_chroma;
};

/// True when `extradata` is meant as an avcC record (version byte 1; Annex B
/// extradata starts with a zero byte).
[[nodiscard]] bool is_avcc(ByteView extradata) noexcept;
[[nodiscard]] Result<AvcConfig> parse_avcc(ByteView record);
/// Build an avcC record from parameter set NAL units (at least one SPS and
/// one PPS). Profile, compatibility and level are copied from the first SPS;
/// the High-profile extension is written when the SPS profile needs it.
[[nodiscard]] Result<Bytes> build_avcc(std::span<const ByteView> sps, std::span<const ByteView> pps,
                                       int length_size = 4);

}  // namespace cvt::h264
