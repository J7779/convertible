// Convertible core - vector family: PostScript-based formats (eps, ps, ai) -
// the reader and writer entry points used by ps_register.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "util/bytes.hpp"

namespace cvt::vector {

enum class PsKind : std::uint8_t { EPS, PS, AI };

/// Document structuring comments of interest.
struct DscInfo {
  Rect bbox;      ///< %%BoundingBox (points)
  Rect hires;     ///< %%HiResBoundingBox
  Rect media;     ///< %%DocumentMedia size (0, 0, w, h)
  bool atend = false;
  int pages = 0;
  std::string title;
};

/// The PostScript part of a DOS EPS file (binary header C5 D0 D3 C6 with
/// TIFF / WMF previews), or the input itself. Empty when the header is bad.
[[nodiscard]] ByteView postscript_section(ByteView input);
[[nodiscard]] DscInfo parse_dsc(std::string_view text);

/// Interpret the program and return the drawing.
[[nodiscard]] Result<Scene> read_postscript(ByteView input, ReadContext& ctx, PsKind kind);

struct PsWriteOptions {
  bool eps = true;
  int level = 2;           ///< LanguageLevel 2 or 3
  bool text_as_text = false;
  int precision = 3;       ///< decimals in coordinates (points)
  double raster_dpi = 150; ///< resolution of rasterised transparency
};

[[nodiscard]] Result<Bytes> write_postscript(const Scene& scene, WriteContext& ctx, const PsWriteOptions& options);

}  // namespace cvt::vector
