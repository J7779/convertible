// Convertible core - vector family: helpers shared by the PostScript writer
// and the PDF content-stream export (number formatting, ASCII85, LZW,
// scene bounds, gradient functions, transparency checks).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "families/vector/scene.hpp"
#include "ir/image.hpp"
#include "util/bytes.hpp"

namespace cvt::vector::emit {

/// Fixed-point number with at most `precision` decimals, trailing zeros
/// removed, never "-0", never exponent notation.
[[nodiscard]] std::string num(double value, int precision);
/// "[a b c d e f]" with `precision` decimals (at least 6 for the linear part).
[[nodiscard]] std::string matrix_array(const Matrix& m, int precision);

/// ASCII85 with "~>" at the end, lines of at most 72 characters.
[[nodiscard]] std::string ascii85(ByteView data);
/// LZW (PostScript / PDF LZWDecode, EarlyChange 1) compressed data.
[[nodiscard]] Bytes lzw_encode(ByteView data);
/// A PostScript string literal "(...)" with the necessary escapes.
[[nodiscard]] std::string ps_string(std::string_view bytes);

/// Bounds of a node / group content in the coordinates of its parent
/// (`m` maps the node's coordinates to the target space). Strokes are
/// padded by half their width; filters by their region or blur radius.
[[nodiscard]] Rect node_bounds(const Node& node, const Matrix& m);
[[nodiscard]] Rect group_content_bounds(const Group& group, const Matrix& m);

/// True when painting needs transparency (opacity, alpha colours,
/// translucent pictures) - something PostScript cannot express.
[[nodiscard]] bool paint_translucent(const Paint& paint);
[[nodiscard]] bool node_translucent(const Node& node);
/// True when a group has effects that need rasterising in PostScript
/// (opacity < 1, mask, filter, blend mode).
[[nodiscard]] bool group_needs_effects(const Group& group);

/// Gradient stops sorted, clamped and padded so that the first is at 0 and
/// the last at 1 (at least two stops).
[[nodiscard]] std::vector<GradientStop> normalized_stops(const Gradient& gradient);

/// A copy of the page root that keeps only the content painted up to and
/// including the node at `path` (indices from the root), so that rendering
/// it shows that node over everything beneath it.
[[nodiscard]] Group prefix_copy(const Group& root, std::span<const std::size_t> path);

/// Picture pixels as packed samples: gray when every pixel is gray, else RGB;
/// alpha is composited over white. `components` receives 1 or 3.
[[nodiscard]] Bytes picture_samples(const ir::Frame& frame, int& components);

/// Number of colour components of a baseline/progressive JPEG (from its SOF
/// marker), 0 when it cannot be determined.
[[nodiscard]] int jpeg_components(ByteView jpeg, std::uint32_t* width = nullptr, std::uint32_t* height = nullptr);

/// The picture's pixels; when the scene only carries encoded bytes (e.g. a
/// scene envelope read without a registry), they are decoded through the
/// image codecs into `storage`. Null when unavailable.
[[nodiscard]] const ir::Frame* picture_frame(const Picture& pic, const Context& ctx, ir::Frame& storage);

}  // namespace cvt::vector::emit
