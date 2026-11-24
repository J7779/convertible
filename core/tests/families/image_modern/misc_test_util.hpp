// Helpers shared by the tests of the assorted image_modern formats (prefix misc).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

namespace cvt::test::misc_util {

/// Bytes of a fixture below core/tests/data/image_modern/misc/.
inline Bytes data(std::string_view name) { return fixture(std::string("image_modern/misc/") + std::string(name)); }

/// Decode a fixture with the registered reader of `format`; FAILs on error.
inline ir::Image load(std::string_view format, std::string_view name, std::vector<std::string>* warnings = nullptr) {
  auto image = read_as<ir::Image>(format, data(name), {}, warnings);
  if (!image) {
    FAIL_CHECK("decoding ", name, ": ", image.error().message);
    return {};
  }
  return std::move(*image);
}

/// A reference picture (PNG made by ImageMagick) decoded by the image family.
inline ir::Frame reference(std::string_view name) {
  const ir::Image image = load("png", name);
  return image.valid() ? image.frames.front() : ir::Frame{};
}

inline ir::Image single(ir::Frame frame) {
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

/// Largest per-channel difference, treating fully transparent pixels as equal
/// whatever their colour.
inline int max_diff_visible(const ir::Frame& a, const ir::Frame& b) {
  if (a.width != b.width || a.height != b.height || !a.valid() || !b.valid()) return 1000;
  int worst = 0;
  for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
    if (a.rgba[i + 3] == 0 && b.rgba[i + 3] == 0) continue;
    for (int c = 0; c < 4; ++c) worst = std::max(worst, std::abs(int{a.rgba[i + c]} - int{b.rgba[i + c]}));
  }
  return worst;
}

/// The id detection picks for the bytes (and file name).
inline std::string detected(ByteView bytes, std::string_view filename = {}) {
  const auto found = detect_format(Registry::instance(), bytes, filename);
  return found && found->format != nullptr ? found->format->id : std::string("(none)");
}

}  // namespace cvt::test::misc_util
