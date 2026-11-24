// Convertible core - image_modern family: JPEG 2000 internals.
//
// Three layers, each in its own file:
//   jp2_box.cpp    in-house JP2/JPX box parser and writer, codestream SIZ parser
//   jp2_opj.cpp    the only code that talks to the imported OpenJPEG library:
//                  codestream <-> sample planes, memory streams only
//   jp2.cpp        registration, sample planes <-> ir::Image (palette, channel
//                  definitions, colour spaces, precisions), options
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::jp2 {

// --------------------------------------------------------------------------- codestream header

/// One component as declared in the SIZ marker segment.
struct SizComponent {
  std::uint32_t precision = 8;  ///< 1..38 bits
  bool is_signed = false;
  std::uint32_t dx = 1;  ///< horizontal sub-sampling, 1..255
  std::uint32_t dy = 1;
};

/// The image and tile geometry of a codestream (ISO/IEC 15444-1 A.5.1).
struct SizInfo {
  std::uint16_t rsiz = 0;
  std::uint32_t x1 = 0, y1 = 0;  ///< Xsiz, Ysiz (reference grid extent)
  std::uint32_t x0 = 0, y0 = 0;  ///< XOsiz, YOsiz (image offset)
  std::uint32_t tile_w = 0, tile_h = 0;
  std::uint32_t tile_x0 = 0, tile_y0 = 0;
  std::vector<SizComponent> components;

  [[nodiscard]] std::uint32_t width() const noexcept { return x1 - x0; }
  [[nodiscard]] std::uint32_t height() const noexcept { return y1 - y0; }
  [[nodiscard]] std::uint64_t tiles() const noexcept;
  /// Samples of all components together (sub-sampling applied).
  [[nodiscard]] std::uint64_t samples() const noexcept;
};

/// Parse SOC + SIZ at the start of a codestream, validating every field.
[[nodiscard]] Result<SizInfo> parse_siz(ByteView codestream);

// --------------------------------------------------------------------------- JP2 / JPX container

/// Colour specification (`colr`).
struct ColourSpec {
  std::uint8_t method = 0;          ///< 1 enumerated, 2 restricted ICC, 3 any ICC (JPX), 4 vendor
  std::uint32_t enumerated = 0;     ///< EnumCS when method == 1
  Bytes icc;                        ///< profile when method is 2 or 3
};

/// Palette (`pclr`): `entries` rows of `columns.size()` values.
struct Palette {
  struct Column {
    std::uint32_t precision = 8;
    bool is_signed = false;
  };
  std::vector<Column> columns;
  std::uint32_t entries = 0;
  /// Row-major: value of entry e, column c at [e * columns + c].
  std::vector<std::int32_t> values;
};

/// Component mapping (`cmap`) entry.
struct ComponentMapping {
  std::uint16_t component = 0;
  std::uint8_t type = 0;    ///< 0 direct use, 1 palette mapping
  std::uint8_t column = 0;  ///< palette column when type == 1
};

/// Channel definition (`cdef`) entry.
struct ChannelDefinition {
  std::uint16_t channel = 0;
  std::uint16_t type = 0;         ///< 0 colour, 1 opacity, 2 premultiplied opacity, 65535 unspecified
  std::uint16_t association = 0;  ///< 0 whole image, 1..n colour n, 65535 none
};

/// What a JP2/JPX file says about its first codestream.
struct FileInfo {
  std::string brand;                 ///< "jp2 ", "jpx ", ...
  std::vector<std::string> compatible;
  bool has_ihdr = false;
  std::uint32_t ihdr_width = 0, ihdr_height = 0;
  std::uint16_t ihdr_components = 0;
  std::vector<ColourSpec> colours;   ///< in file order
  std::optional<Palette> palette;
  std::vector<ComponentMapping> mapping;
  std::vector<ChannelDefinition> channels;
  double dpi_x = 0.0, dpi_y = 0.0;  ///< capture resolution, else display resolution
  Bytes exif;                        ///< from a 'JpgTiffExif->JP2' uuid box (TIFF structure)
  std::string xmp;                   ///< from the XMP uuid box or an XMP 'xml ' box
  ByteView codestream;               ///< contents of the first `jp2c` box
};

/// True when the data starts with the JP2 signature box.
[[nodiscard]] bool has_jp2_signature(ByteView data) noexcept;
/// True when the data starts with SOC followed by SIZ.
[[nodiscard]] bool has_codestream_signature(ByteView data) noexcept;
/// The brand of a JP2-family file ("jp2 ", "jpx ", "jpm "), empty when not one.
[[nodiscard]] std::string file_brand(ByteView data);

/// Parse the boxes of a JP2 or JPX file. Warnings for ignored features go to
/// `ctx`. Fails when there is no usable codestream.
[[nodiscard]] Result<FileInfo> parse_file(ByteView data, const Context& ctx);

/// What the writer puts around a codestream.
struct FileSpec {
  bool jpx = false;  ///< brand 'jpx ' (JP2-compatible) with a reader requirements box
  std::uint32_t width = 0, height = 0;
  std::vector<SizComponent> components;
  std::uint32_t enumerated = 16;     ///< used when `icc` is empty: 16 sRGB, 17 greyscale
  Bytes icc;                         ///< restricted ICC profile (method 2) when not empty
  bool alpha_last = false;           ///< the last component is (non-premultiplied) opacity
  double dpi_x = 0.0, dpi_y = 0.0;   ///< written as capture resolution when > 0
  Bytes exif;                        ///< written in a 'JpgTiffExif->JP2' uuid box
  std::string xmp;                   ///< written in the XMP uuid box
};

/// Build a JP2/JPX file around `codestream`.
[[nodiscard]] Bytes build_file(const FileSpec& spec, ByteView codestream);

// --------------------------------------------------------------------------- OpenJPEG wrapper

/// One decoded component, as produced by the codestream decoder.
struct Plane {
  std::uint32_t dx = 1, dy = 1;  ///< sub-sampling
  std::uint32_t x0 = 0, y0 = 0;  ///< position of sample (0, 0) on the component grid
  std::uint32_t width = 0, height = 0;
  std::uint32_t precision = 8;
  bool is_signed = false;
  const std::int32_t* data = nullptr;  ///< width * height samples, row-major
};

/// A decoded codestream. The planes point into memory owned by `owner`.
struct DecodedPlanes {
  std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  ///< image area on the reference grid
  std::vector<Plane> planes;
  std::shared_ptr<void> owner;
};

/// Decode a raw codestream. `siz` must come from parse_siz on the same bytes
/// and have passed the limit checks; progress is reported in 0..1 of `ctx`.
[[nodiscard]] Result<DecodedPlanes> decode_codestream(ByteView codestream, const SizInfo& siz,
                                                      const Context& ctx);

enum class Progression { LRCP, RLCP, RPCL, PCRL, CPRL };

struct EncodeParams {
  std::uint32_t width = 0, height = 0;
  std::uint32_t components = 3;
  std::uint32_t precision = 8;  ///< 1..16, unsigned
  bool lossless = false;
  /// Lossy only: PSNR target of the final quality layer in dB.
  double psnr = 40.0;
  /// Quality layers, 1..20. Earlier layers get lower PSNR (lossy) or higher
  /// compression ratios (lossless); the last layer meets `psnr` / is lossless.
  int layers = 1;
  /// Tile edge in pixels; 0 = one tile for the whole image.
  std::uint32_t tile_size = 0;
  Progression progression = Progression::LRCP;
  /// Apply the RCT/ICT colour transform to components 0..2 (RGB input).
  bool colour_transform = true;
  /// Fill plane `component` (width * height samples, row-major).
  std::function<void(std::uint32_t component, std::int32_t* samples)> fill;
};

/// Encode samples into a raw codestream.
[[nodiscard]] Result<Bytes> encode_codestream(const EncodeParams& params, const Context& ctx);

}  // namespace cvt::image_modern::jp2
