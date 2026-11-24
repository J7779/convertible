// Convertible core - image family: helpers shared by the format codecs.
// Shared by the raster image families (image and image_modern): option sets,
// metadata and decode/encode plumbing every raster codec needs. Other
// families use image_ops.hpp (pixel operations) and image_api.hpp (codecs).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "families/image/image_ops.hpp"
#include "ir/any.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

// --------------------------------------------------------------------------- option sets

/// `max_width`, `max_height`, `scale`.
[[nodiscard]] std::vector<OptionSpec> size_options();
/// `background` (colour behind transparent areas for formats without alpha).
[[nodiscard]] OptionSpec background_option();
/// The background colour chosen by the user (default white).
[[nodiscard]] Result<Rgb> resolved_background(const Options& options);
/// `dither`: "floyd-steinberg" | "ordered" | "none", for palette output.
[[nodiscard]] OptionSpec dither_option();
[[nodiscard]] Dither resolved_dither(const Options& options);

/// Lossless writer: compression_level, preset, size options, strip_metadata.
[[nodiscard]] std::vector<OptionSpec> lossless_writer_options(int max_level = 9);
/// Lossy writer: quality, preset, size options, strip_metadata.
[[nodiscard]] std::vector<OptionSpec> lossy_writer_options();
/// Uncompressed writer: preset (accepted for uniformity), size options, strip_metadata.
[[nodiscard]] std::vector<OptionSpec> plain_writer_options();

/// Append `more` to `specs` (convenience for building option lists).
[[nodiscard]] std::vector<OptionSpec> with(std::vector<OptionSpec> specs, std::initializer_list<OptionSpec> more);

// --------------------------------------------------------------------------- registration

/// Register one Image codec entry. Pass an empty `read` or `write` for
/// one-directional codecs. `lossy` describes the writer (readers are
/// registered as separate non-lossy entries when the writer is lossy).
struct ImageCodecSpec {
  std::string format_id;
  std::function<Result<AnyIR>(ReadContext&)> read;
  std::function<Result<Bytes>(const AnyIR&, WriteContext&)> write;
  std::vector<OptionSpec> write_options;
  std::vector<OptionSpec> read_options;
  bool lossy_write = false;
  int read_cost = 10;
  int write_cost = 10;
};
void add_image_codec(Registry& registry, ImageCodecSpec spec);

/// Adapters from typed functions to the Codec signatures.
using ImageReader = Result<ir::Image> (*)(ReadContext& ctx);
using ImageWriter = Result<Bytes> (*)(const ir::Image& image, WriteContext& ctx);
[[nodiscard]] std::function<Result<AnyIR>(ReadContext&)> reader(ImageReader fn);
[[nodiscard]] std::function<Result<Bytes>(const AnyIR&, WriteContext&)> writer(ImageWriter fn);

// --------------------------------------------------------------------------- reading

/// Validate decoded dimensions against zero and the pixel limit (all frames).
[[nodiscard]] Result<void> check_size(const Context& ctx, std::uint64_t width, std::uint64_t height,
                                      std::uint64_t frames = 1);
/// Reject a declared picture size that the available data cannot possibly
/// fill: `pixels` must not exceed `data_bytes * max_pixels_per_byte` (the
/// best compression ratio the format allows, with some slack). Stops tiny
/// hostile files from making a decoder allocate gigabytes.
[[nodiscard]] Result<void> check_plausible(std::uint64_t pixels, std::uint64_t data_bytes,
                                           std::uint64_t max_pixels_per_byte);
/// A new frame of the given size, after `check_size`.
[[nodiscard]] Result<ir::Frame> new_frame(const Context& ctx, std::uint32_t width, std::uint32_t height,
                                          Rgba fill = {0, 0, 0, 255});

/// Expand an n-bit sample (1..16 bits) to 8 bits with correct rounding.
[[nodiscard]] constexpr std::uint8_t scale_to_8(std::uint32_t value, int bits) noexcept {
  if (bits == 8) return static_cast<std::uint8_t>(value);
  const std::uint32_t max = (1u << bits) - 1u;
  if (value >= max) return 255;
  return static_cast<std::uint8_t>((value * 255u + max / 2u) / max);
}
/// Scale a 0..max_value sample to 8 bits (rounded; max_value >= 1).
[[nodiscard]] constexpr std::uint8_t scale_max_to_8(std::uint32_t value, std::uint32_t max_value) noexcept {
  if (value >= max_value) return 255;
  return static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) * 255u + max_value / 2u) / max_value);
}

/// MSB-first bit reader over a byte span (PNG/PBM/CCITT/GIF-style packing is
/// LSB-first: see LsbBitReader). Reading past the end yields zero bits and
/// latches `overrun()`.
class MsbBitReader {
 public:
  explicit MsbBitReader(ByteView data) noexcept : data_(data) {}
  [[nodiscard]] std::uint32_t bits(int count) noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) value = (value << 1) | bit();
    return value;
  }
  [[nodiscard]] std::uint32_t bit() noexcept {
    const std::size_t byte = position_ >> 3;
    if (byte >= data_.size()) {
      overrun_ = true;
      ++position_;
      return 0;
    }
    const std::uint32_t value = (std::to_integer<std::uint32_t>(data_[byte]) >> (7 - (position_ & 7))) & 1u;
    ++position_;
    return value;
  }
  void align_to_byte() noexcept { position_ = (position_ + 7) & ~std::size_t{7}; }
  [[nodiscard]] std::size_t bit_position() const noexcept { return position_; }
  void seek_bit(std::size_t position) noexcept { position_ = position; }
  [[nodiscard]] bool overrun() const noexcept { return overrun_; }
  [[nodiscard]] std::size_t size_bits() const noexcept { return data_.size() * 8; }

 private:
  ByteView data_;
  std::size_t position_ = 0;
  bool overrun_ = false;
};

// --------------------------------------------------------------------------- EXIF

/// The facts the family uses from an EXIF block.
struct ExifInfo {
  /// 1..8 (1 when absent or invalid).
  int orientation = 1;
  /// From XResolution/YResolution/ResolutionUnit; 0 when absent.
  double dpi_x = 0.0;
  double dpi_y = 0.0;
  /// Selected IFD0 text tags (ImageDescription, Make, Model, Software,
  /// DateTime, Artist, Copyright), keyed by their EXIF names.
  std::map<std::string, std::string> text;
};

/// Parse a TIFF-structured EXIF block ("II*\0" / "MM\0*"); a leading
/// "Exif\0\0" is skipped. Never reads out of bounds; nullopt when malformed.
[[nodiscard]] std::optional<ExifInfo> parse_exif(ByteView exif);
/// Set the Orientation tag of an EXIF block to 1 in place (no-op when the
/// block has none or is malformed).
void reset_exif_orientation(Bytes& exif) noexcept;

/// Store an EXIF block on a decoded image: records the raw block (with the
/// orientation reset), merges the text tags and dpi, and rotates/flips the
/// pixels upright. Returns the orientation that was applied.
int attach_exif(ir::Image& image, ByteView exif);

/// The 'Exif\0\0' APP1 identifier.
inline constexpr std::string_view exif_app1_prefix{"Exif\0\0", 6};

// --------------------------------------------------------------------------- writing

/// The image a writer should encode: the source with the standard size
/// options applied (a private copy only when something changed).
class WriteImage {
 public:
  [[nodiscard]] const ir::Image& get() const noexcept { return owned_ ? *owned_ : *source_; }
  [[nodiscard]] const ir::Image* operator->() const noexcept { return &get(); }
  [[nodiscard]] const ir::Image& operator*() const noexcept { return get(); }
  /// A mutable copy (made on first use).
  [[nodiscard]] ir::Image& mutate() {
    if (!owned_) owned_ = *source_;
    return *owned_;
  }
  /// True when metadata (EXIF, XMP, ICC, text) should be written.
  [[nodiscard]] bool keep_metadata() const noexcept { return keep_metadata_; }

 private:
  friend Result<WriteImage> prepare_write(const ir::Image& source, const WriteContext& ctx);
  const ir::Image* source_ = nullptr;
  std::optional<ir::Image> owned_;
  bool keep_metadata_ = false;
};

/// Validate the image, apply the size options and resolve `strip_metadata`.
/// Fails with Status::Encode for an invalid image.
[[nodiscard]] Result<WriteImage> prepare_write(const ir::Image& source, const WriteContext& ctx);

/// For single-picture formats: warn when only the first frame is written.
void warn_if_animated(const ir::Image& image, const WriteContext& ctx, std::string_view format_name);

/// The first frame, flattened onto the `background` option colour when the
/// format has no alpha and the frame has transparency (with a warning).
[[nodiscard]] Result<ir::Frame> opaque_frame(const ir::Frame& frame, const WriteContext& ctx,
                                             std::string_view format_name);

}  // namespace cvt::image
