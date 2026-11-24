// Convertible core - codec_hevc family: H.265/HEVC bitstream helpers and the
// single-picture entry points used for HEIF/HEIC images.
//
// Who uses what
//
//   Containers (video family) do not need this header: they reach the codec
//   through `registry.media()` with codec id `codec::id::hevc`. The decoder
//   takes `CodecParams::extradata` = the hvcC box payload and packets as
//   stored in MP4/MOV/MKV (NAL units with big-endian length prefixes of
//   hvcC.lengthSizeMinusOne + 1 bytes). Without extradata, or when packets
//   start with a start code, it reads Annex-B (MPEG-TS, raw .h265). The
//   encoder emits 4-byte length-prefixed samples without parameter sets and
//   an hvcC payload in `extradata()` (available right after `configure`).
//   `annexb_to_length_prefixed` / `length_prefixed_to_annexb` convert both ways.
//
//   HEIF/HEIC (image_modern family): an image item is an `hvcC` property plus
//   item data in the same length-prefixed form. Either
//     a) decoupled, at run time: make the `hevc` VideoDecoder, configure it
//        with `extradata = hvcC payload`, send ONE packet (the item data),
//        `flush()`, then `receive_frame()` once; or
//     b) call `decode_picture(hvcc, item_data)` below (same code path).
//   Grids: decode every tile item (they share one hvcC) and place the tiles.
//   Alpha: an auxiliary item (urn:mpeg:hevc:2015:auxid:1) is usually 4:0:0;
//   monochrome pictures are delivered as 4:2:0 with neutral chroma, so the
//   alpha values are the Y plane (scale 10-bit values with `>> 2`). Use
//   `parse_hvcc` + `parse_sps` to learn the size and bit depth without
//   decoding, and the `ispe`/`clap`/`irot` properties of the item for the
//   final geometry (the conformance window is already applied).
//   Writing HEIC: `encode_picture(frame, options)` returns the hvcC payload and
//   the item data of one intra picture.
//
// Pictures are delivered in their native layout: I420 / I422 / I444 for
// 8-bit streams, I420P10 / I422P10 / I444P10 for 10-bit streams (Main 10,
// Main 4:2:2 10, Main 4:4:4 10), with `color` taken from the SPS VUI.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"
#include "engine/errors.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::hevc {

/// Id of the raw Annex-B elementary-stream format (.h265, .hevc, .265); the
/// format itself (detection, reader, writer) belongs to the video family.
inline constexpr std::string_view stream_format_id = "hevc";

/// Largest picture the decoder and encoder accept: HEVC level 6.2 limits
/// (MaxLumaPs = 35 651 584, each side <= sqrt(8 * MaxLumaPs) = 16 888).
inline constexpr std::uint32_t max_dimension = 16888;
inline constexpr std::uint64_t max_luma_samples = 35'651'584;

// ---------------------------------------------------------------------------
// NAL units
// ---------------------------------------------------------------------------

/// NAL unit types of ITU-T H.265 Table 7-1 that the helpers care about.
enum class NalType : std::uint8_t {
  TrailN = 0,
  TrailR = 1,
  RaslN = 8,
  RaslR = 9,
  BlaWLp = 16,
  BlaWRadl = 17,
  BlaNLp = 18,
  IdrWRadl = 19,
  IdrNLp = 20,
  Cra = 21,
  Vps = 32,
  Sps = 33,
  Pps = 34,
  Aud = 35,
  Eos = 36,
  Eob = 37,
  Fd = 38,
  PrefixSei = 39,
  SuffixSei = 40,
};

/// Type of a NAL unit (its first header byte), 0..63; 255 for an empty view.
[[nodiscard]] constexpr std::uint8_t nal_type(ByteView nal) noexcept {
  return nal.empty() ? 255 : static_cast<std::uint8_t>((std::to_integer<std::uint8_t>(nal[0]) >> 1) & 0x3F);
}
[[nodiscard]] constexpr bool is_vcl(std::uint8_t type) noexcept { return type < 32; }
/// Intra random access point (BLA, IDR, CRA): a keyframe.
[[nodiscard]] constexpr bool is_irap(std::uint8_t type) noexcept { return type >= 16 && type <= 23; }
[[nodiscard]] constexpr bool is_idr(std::uint8_t type) noexcept { return type == 19 || type == 20; }
[[nodiscard]] constexpr bool is_parameter_set(std::uint8_t type) noexcept { return type >= 32 && type <= 34; }

/// True when `data` begins with an Annex-B start code (00 00 01 or 00 00 00 01).
[[nodiscard]] bool starts_with_start_code(ByteView data) noexcept;

/// The NAL units of an Annex-B stream (start codes removed, trailing zero
/// bytes of each unit dropped). Bytes before the first start code are ignored.
[[nodiscard]] std::vector<ByteView> split_annexb(ByteView data);

/// The NAL units of length-prefixed data (`length_size` = 1, 2 or 4 bytes,
/// big-endian). Fails with Status::Decode when a length runs past the end.
[[nodiscard]] Result<std::vector<ByteView>> split_length_prefixed(ByteView data, unsigned length_size);

/// NAL units joined with 4-byte start codes.
[[nodiscard]] Bytes join_annexb(std::span<const ByteView> nals);

/// NAL units joined with `length_size`-byte big-endian lengths; fails with
/// Status::InvalidArgument when a unit does not fit the length field.
[[nodiscard]] Result<Bytes> join_length_prefixed(std::span<const ByteView> nals, unsigned length_size = 4);

[[nodiscard]] Result<Bytes> annexb_to_length_prefixed(ByteView annexb, unsigned length_size = 4);
[[nodiscard]] Result<Bytes> length_prefixed_to_annexb(ByteView data, unsigned length_size = 4);

/// The RBSP of a NAL unit: emulation-prevention bytes (00 00 03) removed.
[[nodiscard]] Bytes unescape_rbsp(ByteView nal);

// ---------------------------------------------------------------------------
// Parameter sets
// ---------------------------------------------------------------------------

/// general_profile_tier_level() of a VPS/SPS, as also stored in hvcC.
struct ProfileTierLevel {
  std::uint8_t profile_space = 0;
  bool tier = false;  ///< false = Main tier, true = High tier
  std::uint8_t profile_idc = 0;  ///< 1 Main, 2 Main 10, 3 Main Still Picture, 4 Range Extensions
  std::uint32_t compatibility_flags = 0;
  std::uint64_t constraint_flags = 0;  ///< the 48 bits after the compatibility flags
  std::uint8_t level_idc = 0;  ///< 30 x level (93 = level 3.1)
  friend bool operator==(const ProfileTierLevel&, const ProfileTierLevel&) = default;
};

/// What the sequence parameter set says about the pictures.
struct SequenceInfo {
  std::uint8_t sps_id = 0;
  std::uint8_t vps_id = 0;
  std::uint8_t max_sub_layers = 1;
  bool temporal_id_nesting = false;
  ProfileTierLevel ptl;
  std::uint8_t chroma_format_idc = 1;  ///< 0 = 4:0:0, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
  bool separate_colour_planes = false;
  std::uint32_t coded_width = 0;   ///< pic_width_in_luma_samples
  std::uint32_t coded_height = 0;
  /// Conformance window, in luma samples.
  std::uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
  /// Size of the output pictures (coded size minus the conformance window).
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t bit_depth_luma = 8;
  std::uint8_t bit_depth_chroma = 8;
  std::uint8_t log2_max_poc_lsb = 4;
  std::uint32_t max_dec_pic_buffering = 1;  ///< of the highest sub-layer
  std::uint32_t max_num_reorder_pics = 0;
  std::uint8_t log2_ctb_size = 4;
  std::uint8_t log2_min_cb_size = 3;
  bool scaling_list_enabled = false;
  bool long_term_refs_present = false;
  std::uint32_t num_short_term_ref_pic_sets = 0;
  // VUI (defaults = "unspecified")
  bool vui_present = false;
  std::uint32_t sar_width = 0, sar_height = 0;
  bool video_full_range = false;
  std::uint8_t colour_primaries = 2;
  std::uint8_t transfer_characteristics = 2;
  std::uint8_t matrix_coefficients = 2;
  bool chroma_loc_present = false;
  std::uint32_t chroma_sample_loc_type = 0;
  std::uint32_t num_units_in_tick = 0;  ///< timing info; 0 when absent
  std::uint32_t time_scale = 0;
  std::uint32_t min_spatial_segmentation = 0;
};

/// Parse an SPS NAL unit (header included). Status::Decode when malformed or
/// outside what HEVC version 2 allows (sizes, bit depths up to 16).
[[nodiscard]] Result<SequenceInfo> parse_sps(ByteView sps_nal);

/// The picture parameter set fields needed to read slice headers.
struct PictureParamsLite {
  std::uint8_t pps_id = 0;
  std::uint8_t sps_id = 0;
  bool dependent_slice_segments_enabled = false;
  bool output_flag_present = false;
  std::uint8_t num_extra_slice_header_bits = 0;
  bool entropy_coding_sync_enabled = false;
  bool tiles_enabled = false;
};
[[nodiscard]] Result<PictureParamsLite> parse_pps_lite(ByteView pps_nal);

/// The pixel format a decoded picture of this sequence is delivered in.
[[nodiscard]] ir::PixelFormat pixel_format(const SequenceInfo& sps) noexcept;
/// Matrix, range and chroma siting from the VUI; unspecified matrices map to
/// BT.601 below 720 lines and BT.709 otherwise.
[[nodiscard]] ir::ColorInfo color_info(const SequenceInfo& sps) noexcept;

// ---------------------------------------------------------------------------
// hvcC (HEVCDecoderConfigurationRecord, ISO/IEC 14496-15 8.3.3)
// ---------------------------------------------------------------------------

struct DecoderConfig {
  ProfileTierLevel ptl;
  std::uint16_t min_spatial_segmentation = 0;
  std::uint8_t parallelism_type = 0;
  std::uint8_t chroma_format_idc = 1;
  std::uint8_t bit_depth_luma = 8;
  std::uint8_t bit_depth_chroma = 8;
  std::uint16_t avg_frame_rate = 0;
  std::uint8_t constant_frame_rate = 0;
  std::uint8_t num_temporal_layers = 1;
  bool temporal_id_nested = false;
  std::uint8_t length_size = 4;  ///< lengthSizeMinusOne + 1: 1, 2 or 4
  /// NAL units of the arrays (VPS, SPS, PPS, SEI, in that order of types).
  std::vector<Bytes> nals;
};

/// Parse an hvcC payload (the box content without its 8-byte header).
[[nodiscard]] Result<DecoderConfig> parse_hvcc(ByteView hvcc);

/// Build an hvcC payload from parameter-set NAL units (VPS, SPS, PPS; prefix
/// SEI units are carried too, other types are ignored). Needs at least one
/// SPS, which supplies profile, chroma format and bit depths. `length_size`
/// is 1, 2 or 4.
[[nodiscard]] Result<Bytes> build_hvcc(std::span<const ByteView> parameter_sets, unsigned length_size = 4);

// ---------------------------------------------------------------------------
// Single pictures (HEIF/HEIC items)
// ---------------------------------------------------------------------------

/// Decode one coded picture: `hvcc` = hvcC payload, `sample` = the item data
/// (length-prefixed NAL units, length size from hvcC). Also accepts an empty
/// `hvcc` with an Annex-B `sample` that carries its own parameter sets.
/// Returns the first picture of the sample. Status::Decode for damaged or
/// unsupported data, Status::Limit beyond `max_pixels`.
[[nodiscard]] Result<ir::VideoFrame> decode_picture(ByteView hvcc, ByteView sample,
                                                    std::uint64_t max_pixels = max_luma_samples);

struct PictureEncodeOptions {
  /// 1..100 as everywhere in Convertible (90 = visually transparent).
  int quality = 90;
  codec::EncodePreset preset = codec::EncodePreset::Balanced;
  /// Signalled in the VUI; also the matrix used when `frame` is not I420.
  ir::ColorInfo color;
};

struct EncodedPicture {
  Bytes hvcc;    ///< hvcC payload (lengthSizeMinusOne = 3)
  Bytes sample;  ///< item data: 4-byte length-prefixed NAL units of one IDR picture
  std::uint32_t width = 0;   ///< coded output size (odd sizes are padded to even)
  std::uint32_t height = 0;
};

/// Encode one intra picture (Main profile, 4:2:0, 8 bit). Frames in another
/// pixel format are converted to I420 with `options.color` first.
[[nodiscard]] Result<EncodedPicture> encode_picture(const ir::VideoFrame& frame, const PictureEncodeOptions& options);

}  // namespace cvt::hevc
