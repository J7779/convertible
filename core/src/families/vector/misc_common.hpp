// Convertible core - vector family: helpers shared by the smaller vector
// formats (VectorDrawable, TikZ, Lottie, PICT). Private to misc_*.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "families/vector/scene.hpp"

namespace cvt {
class Registry;
}

namespace cvt::vector::misc {

void register_vectordrawable(Registry& registry);
void register_tikz(Registry& registry);
void register_lottie(Registry& registry);
void register_pict(Registry& registry);

/// Result of parsing SVG-style path data: the path up to the first error
/// (the SVG error rule: render what was valid) and whether an error occurred.
struct PathParse {
  Path path;
  bool error = false;
};

/// Parse SVG / Android path data (all commands, arcs, implicit repeats,
/// compact numbers such as "1.5.5" and "-1-2"). More than `max_segments`
/// segments fails with Status::Limit.
[[nodiscard]] Result<PathParse> parse_path_data(std::string_view text, std::size_t max_segments);

/// A number with at most `precision` decimals, trailing zeros removed,
/// "-0" written as "0"; `compact` also drops a leading zero (".5", "-.5").
[[nodiscard]] std::string format_number(double value, int precision, bool compact = false);

/// Minified path data (absolute or relative per segment, whichever is
/// shorter; H/V, S/T shorthands; minimal separators). Coordinates are rounded
/// to `precision` decimals without drift.
[[nodiscard]] std::string minify_path_data(const Path& path, int precision);

/// Android colour "#RGB", "#ARGB", "#RRGGBB", "#AARRGGBB" and a few
/// framework colour resources ("@android:color/white"). nullopt otherwise.
[[nodiscard]] std::optional<Color> parse_android_color(std::string_view text);

/// "#AARRGGBB" or "#RRGGBB" when opaque.
[[nodiscard]] std::string android_color_string(const Color& color);

/// A transform written as translate(tx, ty) * rotate(degrees) * scale(sx, sy).
struct Decomposed {
  double tx = 0, ty = 0, rotation = 0, sx = 1, sy = 1;
};
/// nullopt when the matrix has skew (cannot be written that way) or is not finite.
[[nodiscard]] std::optional<Decomposed> decompose(const Matrix& m);

/// The part of the first contour of `path` between fractions `start` and
/// `end` of its length (Android trimPath / Lottie trim semantics: when
/// start > end the piece wraps around). Curves are flattened with `tolerance`.
[[nodiscard]] Path trim_first_contour(const Path& path, double start, double end, double tolerance);

/// The same for every contour (each trimmed independently) or for the whole
/// path as one length (`simultaneous` = false: Lottie "individually").
[[nodiscard]] Path trim_path(const Path& path, double start, double end, double tolerance, bool per_contour);

/// Approximate a sweep (conic) gradient painting `clip` by solid wedges.
/// `stops` sorted by offset; angle 0 along +x, increasing clockwise (y down).
[[nodiscard]] Group sweep_wedges(Point center, const std::vector<GradientStop>& stops, const Path& clip_path,
                                 FillRule rule, double opacity, int wedges = 72);

/// Colour of a gradient at offset t (stops sorted).
[[nodiscard]] Color gradient_color_at(const std::vector<GradientStop>& stops, double t);

/// Average colour of a gradient (for formats without gradients).
[[nodiscard]] Color gradient_average(const Gradient& gradient);

}  // namespace cvt::vector::misc
