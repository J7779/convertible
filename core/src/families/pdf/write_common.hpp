// Convertible core - pdf family: helpers shared by the writers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string_view>

#include "engine/context.hpp"
#include "families/pdf/builder.hpp"
#include "families/pdf/geom.hpp"
#include "ir/image.hpp"

namespace cvt::pdf {

struct ImageEncoding {
  int quality = 90;
  bool lossless = false;
  int compression_level = 9;
};

/// Embed one frame as an image XObject (DCT via the registry for
/// photographs, otherwise Flate with PNG predictors; alpha as /SMask).
[[nodiscard]] Result<Ref> embed_image(PdfBuilder& builder, const ir::Frame& frame, const ImageEncoding& encoding,
                                      const Context& ctx);

/// Paper sizes in points ("a4", "letter", "legal", "a5", "a3").
[[nodiscard]] Rect paper_size(std::string_view name, bool landscape);

}  // namespace cvt::pdf
