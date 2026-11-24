// Convertible core - vector family: SVG reader and writer (internal API).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "util/bytes.hpp"

namespace cvt::vector::svg {

struct ReadOptions {
  /// Keep <title>/<desc> as scene metadata.
  bool keep_metadata = true;
};

/// Parse an SVG document (UTF-8/16 XML, already decompressed) into a scene.
/// External resources are never fetched; only data: URIs are used.
[[nodiscard]] Result<Scene> read_svg(ByteView bytes, const Context& ctx, const ReadOptions& options = {});

struct WriteOptions {
  /// Decimals for coordinates (-1 = shortest exact).
  int precision = 3;
  /// No indentation or line breaks; shortest path syntax.
  bool minify = false;
  /// Write text as <text> elements (true) or as outlines.
  bool text_as_text = true;
  /// Keep <title>/<desc>.
  bool keep_metadata = false;
  /// Keep element ids from the source (editors' ids can carry names).
  bool keep_ids = false;
  /// Picture format for pixels without original encoded bytes ("png").
  std::string image_format = "png";
};

[[nodiscard]] Result<std::string> write_svg(const Scene& scene, const Context& ctx, const WriteOptions& options = {},
                                            std::size_t page = 0);

}  // namespace cvt::vector::svg
