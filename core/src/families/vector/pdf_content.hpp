// Convertible core - vector family: a scene page as PDF page content.
//
// Turns one page of a `Scene` into everything a PDF writer needs to emit
// that page, as plain data - it writes no PDF file and registers no format:
//
//   * `content`   the page content stream (uncompressed operators); the
//                 default user space is PDF points with y up, the first
//                 operator maps scene px (y down) into it
//   * `objects`   indirect objects the content needs: image XObjects and
//                 their soft masks, form XObjects (transparency groups,
//                 soft-mask groups), tiling patterns. Each is a dictionary
//                 in PDF syntax (without /Length) and optional stream bytes,
//                 already encoded as the dictionary's /Filter says.
//   * `resources` the named page resources (XObject, ExtGState, Shading,
//                 Pattern) with their values in PDF syntax.
//
// Cross references between objects and from resources are written as the
// placeholder "{obj N}" (N = index into `objects`); the placeholder
// "{resources}" stands for the page resource dictionary (forms and
// patterns share it). `resolve` replaces both once the writer has chosen
// object numbers:
//
//   auto page = pdf::page_content(scene, 0, ctx);
//   // assign numbers: first_free + i for objects[i]
//   const auto ref = [&](std::size_t i) { return std::format("{} 0 R", first_free + i); };
//   const std::string res = pdf::resolve(page->resources_dict(), ref, "");
//   for (each object) write "<n> 0 obj " + pdf::resolve(obj.dict, ref, res) (+ stream) ...
//   page dictionary: /MediaBox [0 0 width_pt height_pt] /Resources res /Contents <stream of content>
//
// What maps how: paths, non-zero / even-odd fills, strokes (width, caps,
// joins, miter limit, dashes), clip paths (nested, union per rule),
// gradients as axial / radial shadings with stitched exponential functions
// (translucent stops through a luminosity soft mask; reflect / repeat by
// repeating the stops), group opacity and blend modes through transparency
// groups and ExtGState, masks as soft masks (luminance or alpha), tiling
// patterns, pictures as Flate (or DCT pass-through) image XObjects with an
// SMask for alpha, text as glyph outlines. Filters are rasterised through
// `render_group` (an image with an SMask); when the rasteriser is not
// available the filtered content is drawn without its filter and a warning
// is emitted.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "util/bytes.hpp"

namespace cvt::vector::pdf {

/// An indirect object needed by the page content.
struct PdfObject {
  /// The object's dictionary in PDF syntax ("<< /Type /XObject ... >>"),
  /// without /Length for streams. May contain "{obj N}" and "{resources}".
  std::string dict;
  /// Stream data for stream objects (encoded per the dictionary's /Filter).
  std::optional<Bytes> stream;
};

/// One named page resource.
struct PdfResource {
  std::string category;  ///< "XObject", "ExtGState", "Shading" or "Pattern"
  std::string name;      ///< without the slash, e.g. "Im0"
  std::string value;     ///< PDF syntax: "{obj 3}" or an inline dictionary
};

struct PdfPageContent {
  double width_pt = 0;   ///< MediaBox [0 0 width_pt height_pt]
  double height_pt = 0;
  Bytes content;         ///< the content stream, uncompressed
  std::vector<PdfObject> objects;
  std::vector<PdfResource> resources;

  /// "<< /ExtGState << /GS0 ... >> /XObject << ... >> ... >>" (with placeholders).
  [[nodiscard]] std::string resources_dict() const;
};

struct PdfContentOptions {
  int precision = 3;          ///< decimals for coordinates
  double raster_dpi = 150;    ///< resolution of rasterised filters
  int compression_level = 6;  ///< Flate level for image data (0..9)
};

/// Replace "{obj N}" by `ref(N)` and "{resources}" by `resources`.
[[nodiscard]] std::string resolve(std::string_view text, const std::function<std::string(std::size_t)>& ref,
                                  std::string_view resources);

/// The content of page `page_index` of `scene`. Fails with Status::Encode
/// when the page does not exist or has no size, Status::Limit on huge
/// rasterisations, Status::Cancelled through ctx.
[[nodiscard]] Result<PdfPageContent> page_content(const Scene& scene, std::size_t page_index, const Context& ctx,
                                                  const PdfContentOptions& options = {});

}  // namespace cvt::vector::pdf
