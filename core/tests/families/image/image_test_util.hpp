// Helpers shared by the image family tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

namespace cvt::test::image_util {

/// Parse a binary 8-bit PPM ("P6") written by a reference tool, independent
/// of the codec under test. Returns an invalid frame on malformed input.
inline ir::Frame parse_ppm(const Bytes& data) {
  ir::Frame frame;
  const std::string_view text = as_chars(data);
  std::size_t pos = 0;
  const auto next_number = [&]() -> std::uint32_t {
    while (pos < text.size()) {
      if (text[pos] == '#') {
        while (pos < text.size() && text[pos] != '\n') ++pos;
      } else if (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t') {
        ++pos;
      } else {
        break;
      }
    }
    std::uint32_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') value = value * 10 + static_cast<std::uint32_t>(text[pos++] - '0');
    return value;
  };
  if (text.substr(0, 2) != "P6") return frame;
  pos = 2;
  const std::uint32_t width = next_number();
  const std::uint32_t height = next_number();
  const std::uint32_t max = next_number();
  ++pos;
  if (max != 255 || width == 0 || height == 0 || text.size() < pos + std::size_t{width} * height * 3) return frame;
  frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  for (std::size_t i = 0; i < std::size_t{width} * height; ++i) {
    frame.rgba[i * 4] = static_cast<std::uint8_t>(text[pos + i * 3]);
    frame.rgba[i * 4 + 1] = static_cast<std::uint8_t>(text[pos + i * 3 + 1]);
    frame.rgba[i * 4 + 2] = static_cast<std::uint8_t>(text[pos + i * 3 + 2]);
  }
  return frame;
}

inline ir::Image single(ir::Frame frame) {
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

/// A photo-like picture: smooth gradient plus fine grain.
inline ir::Frame photo_frame(std::uint32_t width, std::uint32_t height, std::uint32_t seed = 11) {
  ir::Frame frame = gradient_frame(width, height);
  const Bytes noise = random_bytes(frame.rgba.size(), seed);
  for (std::size_t i = 0; i < frame.rgba.size(); ++i) {
    if (i % 4 == 3) continue;
    const int v = frame.rgba[i] + (std::to_integer<int>(noise[i]) % 25) - 12;
    frame.rgba[i] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
  }
  return frame;
}

/// Decode a fixture with the registered reader of `format`; FAILs the test on error.
inline ir::Image load(std::string_view format, std::string_view fixture_name) {
  auto image = read_as<ir::Image>(format, fixture(std::string(fixture_name)));
  if (!image) {
    FAIL_CHECK("decoding ", fixture_name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

}  // namespace cvt::test::image_util
