// Convertible core - codec_av1 family: AV1 bitstream syntax (OBUs, sequence
// header, av1C, Annex B).
//
// The parsing here is in-house and independent of the imported decoder: it
// is what the wrapper uses to validate input before libaom sees it (frame
// size limits), to describe streams (VideoFrame metadata, av1C) and to
// convert between the two AV1 stream layouts:
//
//   Section 5 ("low overhead")  OBUs with obu_has_size_field = 1, back to back.
//                                Used by MP4/ISOBMFF, Matroska/WebM, IVF and
//                                raw .obu files.
//   Annex B ("length delimited") temporal_unit(size) { frame_unit(size) {
//                                obu_length + OBU }... }. Used by some raw
//                                .obu files and broadcast transports.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::av1 {

/// OBU types (AV1 specification 6.2.2).
enum class ObuType : std::uint8_t {
  Reserved0 = 0,
  SequenceHeader = 1,
  TemporalDelimiter = 2,
  FrameHeader = 3,
  TileGroup = 4,
  Metadata = 5,
  Frame = 6,
  RedundantFrameHeader = 7,
  TileList = 8,
  Padding = 15,
};

/// One OBU located inside a buffer.
struct Obu {
  ObuType type = ObuType::Reserved0;
  bool has_extension = false;
  std::uint8_t temporal_id = 0;
  std::uint8_t spatial_id = 0;
  /// The OBU header byte(s) exactly as stored (1 or 2 bytes).
  ByteView header;
  /// The payload (after the header and the size field, if any).
  ByteView payload;
};

/// Reads an unsigned LEB128 value (at most 8 bytes, value < 2^32).
/// Returns the value and the number of bytes used.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::size_t>> read_leb128(ByteView data);
/// Appends the shortest LEB128 encoding of `value`.
void write_leb128(Bytes& out, std::uint32_t value);

/// Splits a Section 5 buffer into OBUs. Every OBU must carry a size field,
/// except that the last one may omit it (its payload then runs to the end of
/// the buffer). Fails with Status::Decode on any structural error.
[[nodiscard]] Result<std::vector<Obu>> split_section5(ByteView data);

/// Converts one or more Annex B temporal units (back to back) to Section 5:
/// every OBU gets obu_has_size_field = 1 and an explicit size. Fails with
/// Status::Decode when the length fields are inconsistent.
[[nodiscard]] Result<Bytes> annexb_to_section5(ByteView data);

/// True when `data` parses completely as Annex B temporal units whose OBUs
/// have valid headers.
[[nodiscard]] bool is_annexb(ByteView data);
/// True when `data` parses completely as Section 5 OBUs with valid headers
/// and size fields (optionally allowing the last OBU to omit its size).
[[nodiscard]] bool is_section5(ByteView data, bool allow_last_without_size = false);

/// Appends one OBU in Section 5 form (header with obu_has_size_field set,
/// explicit size, payload).
void append_obu(Bytes& out, const Obu& obu);

/// Splits a Section 5 buffer into temporal units: a new unit starts at every
/// temporal delimiter OBU after the first OBU. Temporal delimiters are kept
/// at the start of their unit. A buffer without delimiters is one unit.
[[nodiscard]] Result<std::vector<ByteView>> split_temporal_units(ByteView data);

/// Returns `data` (Section 5) without its temporal delimiter and padding OBUs.
[[nodiscard]] Result<Bytes> strip_temporal_delimiters(ByteView data);

/// The fields of a sequence header OBU (AV1 specification 5.5) that matter to
/// containers and to the pixel format.
struct SequenceHeader {
  std::uint8_t profile = 0;
  bool still_picture = false;
  bool reduced_still_picture_header = false;
  std::uint8_t level_idx0 = 0;  ///< seq_level_idx[0]
  std::uint8_t tier0 = 0;       ///< seq_tier[0]
  bool initial_display_delay_present0 = false;
  std::uint8_t initial_display_delay_minus_one0 = 0;
  std::uint32_t operating_points = 1;
  std::uint32_t max_frame_width = 0;
  std::uint32_t max_frame_height = 0;
  bool use_128x128_superblock = false;
  bool enable_superres = false;
  bool enable_cdef = false;
  bool enable_restoration = false;
  // color_config()
  std::uint8_t bit_depth = 8;
  bool monochrome = false;
  bool color_description_present = false;
  std::uint8_t color_primaries = 2;           ///< CICP, 2 = unspecified
  std::uint8_t transfer_characteristics = 2;  ///< CICP, 2 = unspecified
  std::uint8_t matrix_coefficients = 2;       ///< CICP, 2 = unspecified
  bool full_range = false;
  std::uint8_t subsampling_x = 1;
  std::uint8_t subsampling_y = 1;
  std::uint8_t chroma_sample_position = 0;  ///< 0 unknown, 1 vertical (left), 2 colocated (top-left)
  bool separate_uv_delta_q = false;
  bool film_grain_params_present = false;
};

/// Parses the payload of a sequence header OBU.
[[nodiscard]] Result<SequenceHeader> parse_sequence_header(ByteView payload);

/// Returns the first sequence header OBU (header + payload, Section 5 form)
/// in a Section 5 buffer, if any.
[[nodiscard]] Result<std::optional<Bytes>> find_sequence_header_obu(ByteView data);

/// The AV1CodecConfigurationRecord ('av1C' box payload, also Matroska
/// CodecPrivate) of the AV1 ISOBMFF binding, section 2.3.
struct Av1Config {
  std::uint8_t profile = 0;
  std::uint8_t level_idx0 = 0;
  std::uint8_t tier0 = 0;
  bool high_bitdepth = false;
  bool twelve_bit = false;
  bool monochrome = false;
  std::uint8_t subsampling_x = 1;
  std::uint8_t subsampling_y = 1;
  std::uint8_t chroma_sample_position = 0;
  std::optional<std::uint8_t> initial_presentation_delay_minus_one;
  /// configOBUs: zero or more Section 5 OBUs (normally one sequence header).
  Bytes config_obus;
};

/// Builds an av1C record from a sequence header OBU (Section 5 form, as
/// returned by find_sequence_header_obu).
[[nodiscard]] Result<Bytes> make_av1c(ByteView sequence_header_obu);
/// Parses an av1C record. Fails with Status::Decode when it is malformed.
[[nodiscard]] Result<Av1Config> parse_av1c(ByteView data);

}  // namespace cvt::av1
