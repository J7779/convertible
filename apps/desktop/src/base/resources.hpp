// Convertible desktop - files embedded into the executable at build time
// (fonts, design tokens, motion storyboard, licence texts). The definitions
// are produced by cmake/EmbedResources.cmake in the build directory.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace cvd {

/// Returns the embedded file registered under `name` (e.g. "fonts/Inter-Regular.ttf"),
/// or an empty span.
std::span<const std::uint8_t> resource(std::string_view name);

inline std::string_view resource_text(std::string_view name) {
  const auto data = resource(name);
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

/// Installs the bundled fonts into text::FontSet. Returns false if any is unusable.
bool install_bundled_fonts();

}  // namespace cvd
