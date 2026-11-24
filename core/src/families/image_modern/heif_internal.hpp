// Convertible core - image_modern family: shared declarations of the HEIF
// (ISO/IEC 23008-12) container code used by the AVIF, HEIC and HEIF codecs.
//
//   heif_box.cpp     ISO-BMFF parsing (ftyp, meta, moov) and a box writer
//   heif_color.cpp   decoded pictures <-> RGBA rasters (CICP, ranges, HDR)
//   heif_read.cpp    items, grids, overlays, alpha, transforms, sequences -> ir::Image
//   heif_write.cpp   ir::Image -> AVIF / HEIC files
//   heif.cpp         format and codec registration
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "ir/image.hpp"
#include "ir/video.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern::heif {

// --------------------------------------------------------------------------- four-character codes

[[nodiscard]] constexpr std::uint32_t fourcc(std::string_view code) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value = (value << 8) | static_cast<std::uint8_t>(i < code.size() ? code[i] : ' ');
  }
  return value;
}
/// Printable form of a code for messages ("hvc1"; non-printable bytes as '?').
[[nodiscard]] std::string fourcc_text(std::uint32_t code);

// --------------------------------------------------------------------------- colour description

/// ITU-T H.273 code points (an 'nclx' colour box or a bitstream's VUI).
struct Cicp {
  std::uint16_t primaries = 2;  ///< 2 = unspecified
  std::uint16_t transfer = 2;
  std::uint16_t matrix = 2;
  bool full_range = false;
  friend bool operator==(const Cicp&, const Cicp&) = default;
};

/// Transfer characteristics that carry high dynamic range.
inline constexpr std::uint16_t transfer_pq = 16;
inline constexpr std::uint16_t transfer_hlg = 18;

// --------------------------------------------------------------------------- the parsed container

struct Extent {
  std::uint64_t offset = 0;  ///< absolute (file or idat) offset, base offset included
  std::uint64_t length = 0;  ///< 0 = up to the end of the source
};

struct PropertyLink {
  std::uint16_t index = 0;  ///< 1-based index into BmffFile::properties
  bool essential = false;
};

struct Item {
  std::uint32_t id = 0;
  std::uint32_t type = 0;  ///< 'av01', 'hvc1', 'grid', 'iovl', 'iden', 'Exif', 'mime', ...
  std::string name;
  std::string content_type;  ///< 'mime' items
  std::string content_encoding;
  bool hidden = false;
  bool has_location = false;
  std::uint8_t construction_method = 0;  ///< 0 file, 1 idat, 2 item
  std::uint16_t data_reference_index = 0;
  std::vector<Extent> extents;
  std::vector<PropertyLink> properties;
};

struct Property {
  std::uint32_t type = 0;
  ByteView body;  ///< box content after the 8/16-byte header (full boxes keep version and flags)
};

struct Reference {
  std::uint32_t type = 0;  ///< 'dimg', 'auxl', 'thmb', 'cdsc', 'prem', ...
  std::uint32_t from = 0;
  std::vector<std::uint32_t> to;
};

struct Sample {
  std::uint64_t offset = 0;
  std::uint32_t size = 0;
  std::uint32_t duration = 0;  ///< media timescale ticks
  bool sync = true;
};

/// One track of an image sequence ('pict'/'vide'/'auxv' handler).
struct Track {
  std::uint32_t id = 0;
  std::uint32_t handler = 0;
  std::uint32_t timescale = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t codec = 0;  ///< sample entry type: 'av01', 'hvc1', 'hev1'
  ByteView config;          ///< av1C / hvcC payload
  std::optional<Cicp> nclx;
  ByteView icc;
  std::string aux_type;  ///< from an 'auxi' box ('auxv' tracks)
  std::vector<std::uint32_t> auxl;  ///< track ids this auxiliary track describes
  /// Plays of the sequence from the edit list; 0 = forever / unknown.
  std::uint32_t plays = 0;
  std::vector<Sample> samples;
};

struct BmffFile {
  ByteView data;
  std::uint32_t major_brand = 0;
  std::vector<std::uint32_t> brands;  ///< major and compatible brands
  bool has_meta = false;
  std::vector<Item> items;
  std::vector<Property> properties;
  std::vector<Reference> references;
  std::uint32_t primary = 0;
  ByteView idat;
  std::vector<Track> tracks;

  [[nodiscard]] const Item* item(std::uint32_t id) const noexcept;
  [[nodiscard]] bool has_brand(std::uint32_t brand) const noexcept;
  /// Properties of an item in association order (index 0 links skipped).
  [[nodiscard]] std::vector<std::pair<const Property*, bool>> properties_of(const Item& item) const;
  /// First property of `type` associated with the item, or nullptr.
  [[nodiscard]] const Property* property(const Item& item, std::uint32_t type) const;
  /// Ids `from` refers to with `type`, in order.
  [[nodiscard]] std::vector<std::uint32_t> refs_from(std::uint32_t from, std::uint32_t type) const;
  /// Ids of items that refer to `to` with `type`.
  [[nodiscard]] std::vector<std::uint32_t> refs_to(std::uint32_t to, std::uint32_t type) const;
};

/// Parse the top-level boxes, `meta` and `moov`. Every count and offset is
/// checked against the data and `limits`; never reads out of bounds.
[[nodiscard]] Result<BmffFile> parse_file(ByteView data, const Limits& limits);

/// The bytes of an item (extents concatenated); a view when it is one extent.
struct ItemData {
  ByteView view;
  Bytes owned;
};
[[nodiscard]] Result<ItemData> item_data(const BmffFile& file, const Item& item);

// Typed property payloads ------------------------------------------------------

struct Ispe {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};
struct Clap {
  std::int64_t width_n = 0, width_d = 1, height_n = 0, height_d = 1;
  std::int64_t horiz_n = 0, horiz_d = 1, vert_n = 0, vert_d = 1;
};
/// Crop rectangle of a clean aperture on a `width` x `height` picture; nullopt
/// when it is invalid or lies outside the picture.
struct CropRect {
  std::uint32_t left = 0, top = 0, width = 0, height = 0;
};
[[nodiscard]] std::optional<CropRect> clap_rect(const Clap& clap, std::uint32_t width, std::uint32_t height);

[[nodiscard]] std::optional<Ispe> parse_ispe(ByteView body);
[[nodiscard]] std::optional<Clap> parse_clap(ByteView body);
/// Bits per channel from 'pixi'; empty when malformed.
[[nodiscard]] std::vector<std::uint8_t> parse_pixi(ByteView body);
/// 'colr' payload: either an nclx/nclc code-point set or an ICC profile.
struct Colr {
  std::optional<Cicp> nclx;
  ByteView icc;
};
[[nodiscard]] std::optional<Colr> parse_colr(ByteView body);
/// The auxiliary type URN of an 'auxC' property / 'auxi' box.
[[nodiscard]] std::string parse_aux_type(ByteView body);

// --------------------------------------------------------------------------- box writer

/// Builds nested boxes; `begin` returns a mark that `end` closes (sizes are
/// patched in). Always uses 32-bit sizes, so boxes stay below 4 GiB.
class BoxWriter {
 public:
  std::size_t begin(std::string_view type);
  std::size_t begin_full(std::string_view type, std::uint8_t version, std::uint32_t flags);
  [[nodiscard]] Result<void> end(std::size_t mark);
  util::ByteWriter& out() noexcept { return out_; }
  [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
  [[nodiscard]] Bytes take() noexcept { return out_.take(); }

 private:
  util::ByteWriter out_;
};

// --------------------------------------------------------------------------- rasters

/// An RGBA picture between decoding and the IR: 8-bit samples, or 16-bit
/// samples (0..65535) when the source had more than 8 bits. Colour values
/// are still encoded with the source's transfer function and primaries.
struct Raster {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool wide = false;  ///< samples in p16 instead of p8
  std::vector<std::uint8_t> p8;
  std::vector<std::uint16_t> p16;
  bool has_alpha = false;
  int source_bits = 8;
  Cicp color;  ///< primaries and transfer of the RGB values
  bool identity_matrix = false;

  [[nodiscard]] std::size_t samples() const noexcept { return std::size_t{width} * height * 4; }
};

/// A blank raster (opaque black, or `fill` in 16-bit units) after a pixel-limit check.
[[nodiscard]] Result<Raster> make_raster(const Context& ctx, std::uint32_t width, std::uint32_t height, bool wide,
                                         const std::uint16_t* fill16 = nullptr);
void promote_to_wide(Raster& raster);
[[nodiscard]] Raster crop(const Raster& raster, const CropRect& rect);
/// Rotate anti-clockwise by `quarter_turns` * 90 degrees ('irot').
[[nodiscard]] Raster rotate_ccw(const Raster& raster, int quarter_turns);
/// 'imir' as libheif and Apple implement it: axis 0 exchanges top and bottom,
/// axis 1 exchanges left and right.
[[nodiscard]] Raster mirror(const Raster& raster, int axis);
/// Copy `tile` into `canvas` at (x, y), clipped. Both must have the same width flag.
void blit(Raster& canvas, const Raster& tile, std::int64_t x, std::int64_t y);
/// Alpha-composite `layer` over `canvas` at (x, y) (straight alpha, encoded values).
void composite(Raster& canvas, const Raster& layer, std::int64_t x, std::int64_t y);

/// What the colour conversion needs to know about a decoded picture.
struct PictureInfo {
  int bit_depth = 8;  ///< coded bit depth (8, 10, 12)
  bool monochrome = false;
  Cicp cicp;  ///< after the 'colr' override
  ir::ChromaSiting siting = ir::ChromaSiting::Left;
  /// Repeat each subsampled chroma sample over the pixels it covers instead
  /// of interpolating (what libheif and Apple's decoder do).
  bool nearest_chroma = false;
};
/// YCbCr (or GBR) picture -> RGBA raster. `as_alpha` returns the luma plane
/// (range-expanded) in every channel, for alpha auxiliary images.
[[nodiscard]] Result<Raster> picture_to_raster(const ir::VideoFrame& frame, const PictureInfo& info, bool as_alpha,
                                               const Context& ctx);
/// Store the alpha raster's first channel as the colour raster's alpha.
/// Sizes must match. `premultiplied` divides the colour by alpha.
void attach_alpha(Raster& color, const Raster& alpha, bool premultiplied);

/// The finished image: transfer function, primaries and HDR handling.
/// `icc` non-empty means the RGB values are described by that profile.
[[nodiscard]] Result<ir::Frame> raster_to_frame(const Raster& raster, bool have_icc,
                                                std::optional<ir::HdrPixels>* hdr);

// --------------------------------------------------------------------------- reading and writing

struct ReadOptions {
  bool keep_hdr = true;  ///< fill ir::Image::hdr for sources above 8 bits
};
[[nodiscard]] Result<ir::Image> read_heif(ReadContext& ctx, std::string_view format_name);

enum class Flavor { Avif, Heic, Heif };
[[nodiscard]] Result<Bytes> write_heif(const ir::Image& image, WriteContext& ctx, Flavor flavor);

/// Confidence that `data` is a generic HEIF file (major brand mif1/msf1
/// without a more specific image brand): 0 or 90.
[[nodiscard]] int sniff_generic_heif(ByteView data);

}  // namespace cvt::image_modern::heif
