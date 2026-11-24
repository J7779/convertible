// Convertible core - Image IR: raster frames.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::ir {

/// One raster frame, always 8-bit RGBA.
struct Frame {
  /// Size in pixels. A valid frame has both > 0.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// `width * height * 4` bytes, rows top to bottom, pixels left to right,
  /// channel order R, G, B, A. Colour values are sRGB-encoded; alpha is
  /// straight (NOT premultiplied), 255 = opaque.
  std::vector<std::uint8_t> rgba;
  /// How long the frame is shown in an animation, in milliseconds.
  /// 0 for still images.
  std::uint32_t delay_ms = 0;

  /// A frame of the given size filled with one colour (default transparent black).
  [[nodiscard]] static Frame filled(std::uint32_t width, std::uint32_t height,
                                    std::array<std::uint8_t, 4> rgba = {0, 0, 0, 0}) {
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
      frame.rgba[i] = rgba[0];
      frame.rgba[i + 1] = rgba[1];
      frame.rgba[i + 2] = rgba[2];
      frame.rgba[i + 3] = rgba[3];
    }
    return frame;
  }
  /// True when the buffer size matches the dimensions and both are non-zero.
  [[nodiscard]] bool valid() const noexcept {
    return width > 0 && height > 0 && rgba.size() == static_cast<std::size_t>(width) * height * 4;
  }
  /// Offset of pixel (x, y) in `rgba`. No bounds check.
  [[nodiscard]] std::size_t offset(std::uint32_t x, std::uint32_t y) const noexcept {
    return (static_cast<std::size_t>(y) * width + x) * 4;
  }
  /// True when any pixel is not fully opaque.
  [[nodiscard]] bool has_transparency() const noexcept {
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
      if (rgba[i] != 255) return true;
    }
    return false;
  }
};

/// High dynamic range pixels for sources that carry more than 8 bits of range.
struct HdrPixels {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  /// `width * height * 4` floats, same layout as `Frame::rgba`, but colour is
  /// LINEAR light (1.0 = SDR white, values above 1 allowed) and alpha is
  /// straight in 0..1.
  std::vector<float> rgba;
};

/// A still image or an animation.
struct Image {
  /// At least one frame for a valid image. All frames of an animation are
  /// full-canvas and share the size of `frames[0]` (readers composite partial
  /// frames before handing them over).
  std::vector<Frame> frames;
  /// Number of times an animation plays; 0 = forever. Ignored for stills.
  std::uint32_t loop_count = 0;
  /// Present only when the source was HDR/float (EXR, Radiance HDR, 16-bit
  /// PNG...). Describes the same picture as `frames[0]`, which readers must
  /// still fill with a tone-mapped 8-bit rendition so every writer works.
  std::optional<HdrPixels> hdr;
  /// Pixel density in dots per inch; 0 = unknown.
  double dpi_x = 0.0;
  double dpi_y = 0.0;
  /// Free-form textual metadata ("Title", "Author", "Software", EXIF-derived
  /// keys...). Keys are case-sensitive; values are UTF-8.
  std::map<std::string, std::string> metadata;
  /// Embedded ICC colour profile, verbatim; empty when the image is plain sRGB
  /// or the profile is unknown.
  Bytes icc_profile;
  /// Raw EXIF block as stored in a JPEG APP1 segment after the "Exif\0\0"
  /// prefix (a TIFF structure starting with "II*\0" or "MM\0*"); empty when
  /// absent. Readers apply the EXIF orientation to the pixels and reset the
  /// orientation tag in this copy to 1 ("upright"), so frames are always
  /// stored the way they are meant to be seen.
  Bytes exif;
  /// Raw XMP packet (UTF-8 XML); empty when absent.
  std::string xmp;
  /// Precision of the source samples per channel: 1..16 for integer data,
  /// 32 for floating point. A hint for writers that can store more than 8
  /// bits (16-bit PNG/TIFF/PSD, PFM ...), which then prefer `hdr` when it is
  /// present. 8 when unknown.
  std::uint8_t source_bit_depth = 8;
  /// True when the source stored a single grey channel (plus optional alpha).
  /// A hint only; writers must still look at the pixels.
  bool source_grayscale = false;
  /// The source file's encoded bytes, kept when a writer can embed them
  /// unchanged (a JPEG placed in a PDF as DCTDecode, without recompression),
  /// with their MIME type ("image/jpeg"). Empty when not available. Readers
  /// fill them only when the bytes show exactly `frames[0]` (no orientation
  /// applied, a single 8-bit picture) and the file is small enough; ANY
  /// operation that changes the pixels must clear both (`clear_original()`).
  Bytes original_bytes;
  std::string original_mime;

  void clear_original() noexcept {
    original_bytes.clear();
    original_mime.clear();
  }

  [[nodiscard]] std::uint32_t width() const noexcept { return frames.empty() ? 0 : frames.front().width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return frames.empty() ? 0 : frames.front().height; }
  [[nodiscard]] bool animated() const noexcept { return frames.size() > 1; }
  /// True when there is at least one frame and every frame is valid.
  [[nodiscard]] bool valid() const noexcept {
    if (frames.empty()) return false;
    for (const Frame& frame : frames) {
      if (!frame.valid()) return false;
    }
    return true;
  }
};

}  // namespace cvt::ir
