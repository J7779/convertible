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
