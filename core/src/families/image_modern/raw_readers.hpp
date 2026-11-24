// Convertible core - image_modern family: RAW container readers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/image_modern/raw_ljpeg.hpp"
#include "families/image_modern/raw_model.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::raw {

/// What the caller asked for.
struct Request {
  DevelopOptions develop;
  /// Return the largest embedded preview instead of developing the RAW data.
  bool preview = false;
};

/// What kind of RAW file the bytes are (content only; `filename` refines
/// ids that share a structure, such as nef/nrw). Empty when unknown.
/// `certain` is false when only weak evidence (maker name without RAW data
/// directory) was found.
struct Identified {
  std::string id;
  bool certain = false;
};
[[nodiscard]] Identified identify(ByteView data, std::string_view filename);

/// Adobe DNG (and DNG-based formats such as GoPro GPR).
[[nodiscard]] Result<ir::Image> read_dng(const TiffTree& tree, const Request& request, const Context& ctx);

/// Maker formats: dispatch on the identified id.
[[nodiscard]] Result<ir::Image> read_maker(std::string_view id, ByteView data, const Request& request,
                                           const Context& ctx);

/// Develop a RAW IFD the generic way (uncompressed / lossless JPEG CFA data
/// described by TIFF-EP tags), filling the colour description from `info`.
[[nodiscard]] Result<ir::Image> develop_tiff_raw(const TiffTree& tree, const Ifd& raw_ifd, RawInfo info,
                                                 const Request& request, const Context& ctx);

/// The colour filter pattern of a TIFF-EP / DNG directory (CFARepeatPatternDim
/// + CFAPattern mapped through CFAPlaneColor, or the EXIF CFAPattern form).
/// Colour codes above 2 (cyan, magenta, ...) are kept as is.
[[nodiscard]] std::optional<Cfa> tiff_cfa(const TiffData& tiff, const Ifd& ifd);

/// Decode a Sony ARW2 ("cRAW") strip: 16-byte blocks of 16 same-colour pixels
/// with 11-bit min/max and 7-bit deltas, expanded through the tone curve.
/// `curve_points` are the four knots of tag 0x7010 (empty = linear).
[[nodiscard]] Result<Sensor> decode_arw2(ByteView data, std::uint32_t width, std::uint32_t height,
                                         std::span<const std::uint64_t> curve_points, const Context& ctx);

/// Reassemble a Canon CR2 lossless JPEG frame cut into vertical slices
/// (tag 0xC640: count of full slices, their width, width of the last one).
[[nodiscard]] Result<Sensor> unslice_cr2(const LjpegFrame& frame, std::span<const std::uint64_t> slices);

/// Return the largest preview with a warning that explains why.
[[nodiscard]] Result<ir::Image> preview_fallback(const std::vector<Preview>& previews, std::string_view reason,
                                                 const Context& ctx);

}  // namespace cvt::image_modern::raw
