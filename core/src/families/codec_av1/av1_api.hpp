// Convertible core - codec_av1 family: single-picture AV1 decode and encode,
// for still-image formats built on AV1 (AVIF) and for any other in-core user
// that holds one AV1 picture rather than a stream.
//
// Video goes through the registered codec::VideoDecoder / VideoEncoder for
// codec::id::av1 (registry.media()); this header is for stills.
//
// Decoding an AVIF item
//
//   // `item` = the bytes of the 'av01' item (Section 5 OBUs), `av1c` = the
//   // payload of its 'av1C' property (may be empty).
//   CVT_TRY_ASSIGN(av1::Picture color, av1::decode_still(item, av1c, {.max_pixels = limits.max_pixels}));
//   // color.frame: I420/I422/I444 (8 bit) or I420P10/I422P10/I444P10;
//   // color.color: CICP code points from the sequence header (override with
//   // the 'colr' box when present); color.monochrome for grey/alpha items.
//   // An alpha item decodes the same way: use color.frame.planes[0] (Y) and
//   // color.color.full_range.
//
// Encoding an AVIF item
//
//   av1::StillEncodeOptions options;
//   options.quality = 90;                       // 1..100; or options.lossless = true
//   options.color = {.color_primaries = 1, .transfer_characteristics = 13,
//                    .matrix_coefficients = 6, .full_range = true};
//   CVT_TRY_ASSIGN(av1::EncodedStill still, av1::encode_still(yuv_frame, options));
//   // still.obus -> item data, still.av1c -> 'av1C' property.
//
//   av1::StillEncodeOptions alpha;              // alpha auxiliary item
//   alpha.monochrome = true;                    // only plane 0 is coded
//   alpha.color.full_range = true;
//   CVT_TRY_ASSIGN(av1::EncodedStill a, av1::encode_still(alpha_frame, alpha));
//
// Lossless RGB: pass an I444 (or I444P10) frame whose planes hold G, B, R and
// set color.matrix_coefficients = 0 (identity) with lossless = true; or pass
// an RGBA8 frame with lossless = true and matrix_coefficients = 0, which is
// packed that way here.
//
// All functions are re-entrant; nothing is shared between calls.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "codec/codec.hpp"
#include "engine/errors.hpp"
#include "families/codec_av1/obu.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::av1 {

/// Colour description in ITU-T H.273 (CICP) code points, as coded in the AV1
/// sequence header. 2 means "unspecified".
struct PictureColor {
  std::uint8_t color_primaries = 2;
  std::uint8_t transfer_characteristics = 2;
  std::uint8_t matrix_coefficients = 2;
  bool full_range = false;
  /// 0 unknown, 1 vertical (co-sited with the left column), 2 co-located (top-left).
  std::uint8_t chroma_sample_position = 0;
  friend bool operator==(const PictureColor&, const PictureColor&) = default;
};

/// One decoded AV1 picture.
struct Picture {
  /// The samples, tightly packed:
  ///   8-bit streams:  I420, I422 or I444;
  ///   10-bit streams: I420P10, I422P10 or I444P10;
  ///   12-bit streams: the 10-bit formats, rounded to 10 bits;
  ///   monochrome:     I420 / I420P10 with neutral chroma planes.
  /// Identity-matrix (GBR) streams are left as they are coded (planes G, B, R
  /// in an I444 layout); `frame.color` is then meaningless.
  /// `frame.color` holds the matrix, range and siting derived from `color`.
  ir::VideoFrame frame;
  /// Coded bit depth (8, 10 or 12) and chroma layout.
  int bit_depth = 8;
  bool monochrome = false;
  PictureColor color;
};

struct StillDecodeOptions {
  /// Refuse pictures larger than this (Status::Limit), checked against the
  /// sequence header before anything is allocated.
  std::uint64_t max_pixels = std::uint64_t{1} << 28;
  /// Apply film grain synthesis (normative output); false gives the
  /// un-grained reconstruction.
  bool apply_film_grain = true;
};

/// Decodes the first shown picture of an AV1 bitstream: Section 5 OBUs (an
/// AVIF item, an IVF frame, a .obu file) or Annex B. `av1c` is an optional
/// AV1CodecConfigurationRecord whose configOBUs are used when the data
/// carries no sequence header of its own. Fails with Status::Decode for
/// malformed data and Status::Limit for oversized pictures.
[[nodiscard]] Result<Picture> decode_still(ByteView data, ByteView av1c = {}, const StillDecodeOptions& options = {});

struct StillEncodeOptions {
  /// 1..100, the shared meaning of the `quality` option (90 = visually
  /// transparent). Ignored when `lossless`.
  int quality = 90;
  /// Mathematically lossless in the coded YCbCr (or GBR) domain.
  bool lossless = false;
  /// Speed/size trade-off.
  codec::EncodePreset preset = codec::EncodePreset::Balanced;
  /// Code only plane 0 (an AVIF alpha or grey image).
  bool monochrome = false;
  /// What the sequence header signals. Also selects the matrix used when an
  /// RGBA8 frame has to be converted (1 BT.709, 9 BT.2020, 0 identity,
  /// anything else BT.601) and its range.
  PictureColor color;
};

struct EncodedStill {
  /// Section 5 OBUs without temporal delimiter: the sequence header followed
  /// by the frame (what an AVIF 'av01' item holds).
  Bytes obus;
  /// The matching AV1CodecConfigurationRecord ('av1C' payload).
  Bytes av1c;
  SequenceHeader sequence;
};

/// Encodes one picture as an AV1 still (intra-only, reduced still-picture
/// header). Accepts I420, I422, I444 and their P10 forms directly (4:2:2 and
/// 4:4:4 select AV1 profiles 2 and 1), NV12/P010 and RGBA8 by conversion.
/// Odd sizes are fine. Fails with Status::InvalidArgument for invalid frames.
[[nodiscard]] Result<EncodedStill> encode_still(const ir::VideoFrame& frame, const StillEncodeOptions& options = {});

/// Maps the shared 1..100 quality scale to libaom's 0..63 quantiser level.
[[nodiscard]] int quality_to_cq_level(int quality);

/// Prepends a temporal delimiter OBU, for writers of IVF and raw .obu files
/// (encoder packets, like MP4 and Matroska samples, carry none).
[[nodiscard]] Bytes with_temporal_delimiter(ByteView temporal_unit);

}  // namespace cvt::av1
