// Convertible core - codec_vpx family: VP8/VP9 frame headers, VP9
// superframes and the VP codec configuration record ("vpcC").
//
// These parsers read only the uncompressed part of a frame header. The
// decoder wrappers use them to check sizes before libvpx allocates anything,
// and the encoders to describe their output; they are pure functions over
// bytes and are unit tested on their own.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "engine/errors.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::vpx {

/// VP9 `color_space` codes (VP9 bitstream specification, section 7.2.2).
enum class Vp9ColorSpace : std::uint8_t {
  Unknown = 0,
  Bt601 = 1,
  Bt709 = 2,
  Smpte170 = 3,
  Smpte240 = 4,
  Bt2020 = 5,
  Reserved = 6,
  Srgb = 7,
};

/// What the uncompressed header of one VP8 or VP9 frame says.
struct FrameHeader {
  /// VP9 profile 0..3, or the VP8 version number 0..3.
  int profile = 0;
  bool keyframe = false;
  /// VP9: an intra-only (non-key) frame.
  bool intra_only = false;
  /// False for hidden frames (VP8/VP9 alternate reference frames).
  bool show_frame = true;
  /// VP9: the frame only re-displays an already decoded reference.
  bool show_existing_frame = false;
  /// Coded frame size; 0 when this frame reuses a reference's size (VP9
  /// inter frames) and the size is not known to the parser.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// VP9 colour configuration; only meaningful when `has_color_config`.
  bool has_color_config = false;
  int bit_depth = 8;
  Vp9ColorSpace color_space = Vp9ColorSpace::Unknown;
  bool full_range = false;
  int subsampling_x = 1;
  int subsampling_y = 1;
  /// VP9: which of the 8 reference slots this frame refreshes.
  std::uint8_t refresh_frame_flags = 0;
};

/// Parse the frame tag and, for keyframes, the start code and size of a VP8
/// frame (RFC 6386, section 9.1).
[[nodiscard]] Result<FrameHeader> parse_vp8_header(ByteView frame);

/// Sizes of the 8 VP9 reference slots, needed to know the size of inter
/// frames that copy a reference's size. Update it with `parse_vp9_header`.
struct Vp9RefSizes {
  std::array<std::uint32_t, 8> width{};
  std::array<std::uint32_t, 8> height{};
};

/// Parse the uncompressed header of one VP9 frame (not a superframe; see
/// `split_vp9_superframe`). When `refs` is given, inter-frame sizes are
/// resolved through it and it is updated with the frame's refreshes.
[[nodiscard]] Result<FrameHeader> parse_vp9_header(ByteView frame, Vp9RefSizes* refs = nullptr);

/// Split a VP9 packet into its frames. A packet without a valid superframe
/// index is one frame. An index whose sizes do not add up is an error.
[[nodiscard]] Result<std::vector<ByteView>> split_vp9_superframe(ByteView packet);

/// The fields of a VPCodecConfigurationRecord (VP codec ISO media file
/// format binding, version 1).
struct VpcConfig {
  std::uint8_t profile = 0;
  /// VP9 level times ten (10 = level 1, 41 = level 4.1); 0 when unknown.
  std::uint8_t level = 0;
  std::uint8_t bit_depth = 8;
  /// 0: 4:2:0 vertical (left) siting, 1: 4:2:0 co-located with luma (0,0),
  /// 2: 4:2:2, 3: 4:4:4.
  std::uint8_t chroma_subsampling = 0;
  bool full_range = false;
  /// ISO/IEC 23091-4 / ITU-T H.273 code points.
  std::uint8_t colour_primaries = 2;
  std::uint8_t transfer_characteristics = 2;
  std::uint8_t matrix_coefficients = 2;
  friend bool operator==(const VpcConfig&, const VpcConfig&) = default;
};

/// The payload of a `vpcC` box (version 1 full box: version, flags, record,
/// no codec initialisation data), which is what the encoders return as
/// extradata.
[[nodiscard]] Bytes make_vpcc(const VpcConfig& config);
/// Accepts the `vpcC` payload with its version/flags header (version 1).
[[nodiscard]] Result<VpcConfig> parse_vpcc(ByteView payload);

/// The smallest VP9 level (times ten) whose picture size and luma sample
/// rate admit `width` x `height` at `fps` frames per second (`fps` <= 0:
/// picture size only). 0 when even level 6.2 is too small.
[[nodiscard]] std::uint8_t vp9_level(std::uint32_t width, std::uint32_t height, double fps);

/// ITU-T H.273 code points for a colour description.
struct H273Colour {
  std::uint8_t primaries = 2;
  std::uint8_t transfer = 2;
  std::uint8_t matrix = 2;
};
[[nodiscard]] H273Colour h273_colour(ir::ColorMatrix matrix, int bit_depth);

}  // namespace cvt::vpx
