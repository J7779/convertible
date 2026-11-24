// Convertible core - vector family: SVG/CSS value syntax (numbers, lengths,
// colours, transforms, path data, preserveAspectRatio) and a small CSS
// style-sheet parser.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "families/vector/scene.hpp"

namespace cvt::vector::svg {

/// Cursor over attribute text with SVG number grammar.
struct Scanner {
  std::string_view text;
  std::size_t pos = 0;

  void skip_space();
  void skip_space_comma();
  [[nodiscard]] bool at_end() const { return pos >= text.size(); }
  [[nodiscard]] char peek() const { return pos < text.size() ? text[pos] : '\0'; }
  std::optional<double> number();
  /// Arc flags may be written without separators ("a1 1 0 00.5.5").
  std::optional<bool> flag();
};

/// All numbers in a list ("1, 2 3e1").
[[nodiscard]] std::vector<double> parse_numbers(std::string_view text);

enum class Unit : std::uint8_t { None, Px, Pt, Pc, Mm, Cm, In, Em, Ex, Rem, Percent };
struct Length {
  double value = 0;
  Unit unit = Unit::None;
};
[[nodiscard]] std::optional<Length> parse_length(std::string_view text);
/// Lengths in a list (stroke-dasharray, x="1 2 3").
[[nodiscard]] std::vector<Length> parse_lengths(std::string_view text);

/// A parsed colour; `current` for currentColor.
struct ColorValue {
  Color color;
  bool current = false;
};
[[nodiscard]] std::optional<ColorValue> parse_color(std::string_view text);

[[nodiscard]] std::optional<Matrix> parse_transform(std::string_view text);

/// Path data with every command; stops at the first error (as the spec says)
/// and reports it through `error`.
[[nodiscard]] Path parse_path_data(std::string_view text, bool* error = nullptr, std::size_t max_segments = 50'000'000);

struct AspectRatio {
  enum class Align : std::uint8_t { None, Min, Mid, Max };
  Align x = Align::Mid, y = Align::Mid;
  bool slice = false;
};
[[nodiscard]] AspectRatio parse_aspect_ratio(std::string_view text);
/// The transform that maps `view_box` into the viewport (x, y, w, h).
[[nodiscard]] Matrix view_box_transform(const Rect& view_box, const AspectRatio& ar, double x, double y, double w,
                                        double h);

/// `url(#id)` / `url("#id")` -> "id"; empty when not a local reference.
[[nodiscard]] std::string url_reference(std::string_view text, std::string_view* rest = nullptr);

// --------------------------------------------------------------------------- CSS

struct CssDeclaration {
  std::string property;  ///< lower case
  std::string value;
  bool important = false;
};

/// One compound selector step: type, id, classes, attribute tests.
struct CssCompound {
  std::string type;  ///< "" or "*" = any
  std::string id;
  std::vector<std::string> classes;
  std::vector<std::pair<std::string, std::string>> attributes;  ///< name, value ("" = presence)
  bool first_child = false;
};

struct CssSelector {
  /// Rightmost first. combinators[i] joins parts[i] to parts[i + 1]: ' ' descendant, '>' child.
  std::vector<CssCompound> parts;
  std::vector<char> combinators;
  int specificity = 0;
};

struct CssRule {
  std::vector<CssSelector> selectors;
  std::vector<CssDeclaration> declarations;
  std::size_t order = 0;
};

/// Parse a style sheet; at-rules (@media, @font-face, @import ...) are skipped.
void parse_stylesheet(std::string_view text, std::vector<CssRule>& rules, std::size_t& order);
/// Parse a `style` attribute.
[[nodiscard]] std::vector<CssDeclaration> parse_declarations(std::string_view text);

/// Decode a data: URI; nullopt when not one (or malformed). `mime` receives the media type.
[[nodiscard]] std::optional<Bytes> decode_data_uri(std::string_view uri, std::string* mime);

}  // namespace cvt::vector::svg
