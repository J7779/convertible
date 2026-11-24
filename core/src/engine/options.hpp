// Convertible core - conversion options: schema (OptionSpec) and values (Options).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "util/json.hpp"

namespace cvt {

enum class OptionType {
  Int,     ///< whole number; `min`/`max` bound it
  Float,   ///< real number; `min`/`max` bound it
  Bool,    ///< true / false
  Enum,    ///< one of `choices`
  String,  ///< free text
};

[[nodiscard]] constexpr std::string_view option_type_name(OptionType type) noexcept {
  switch (type) {
    case OptionType::Int: return "int";
    case OptionType::Float: return "float";
    case OptionType::Bool: return "bool";
    case OptionType::Enum: return "enum";
    case OptionType::String: return "string";
  }
  return "string";
}

/// How prominently a UI should show an option.
enum class OptionCategory {
  Basic,     ///< show by default
  Advanced,  ///< tuck away behind "more options"
};

[[nodiscard]] constexpr std::string_view option_category_name(OptionCategory category) noexcept {
  return category == OptionCategory::Advanced ? "advanced" : "basic";
}

/// Declares one user-tunable setting of a codec or bridge.
/// The specs of the chosen route are published through
/// `cvt_options_schema_json` so that every UI can render controls generically,
/// and the engine uses them to validate, coerce, clamp and default the values
/// before a codec sees them.
struct OptionSpec {
  /// Machine name, snake_case, unique within one conversion route
  /// (e.g. "quality", "line_width").
  std::string key;
  /// Short human label in English, sentence case (e.g. "Line width").
  std::string label;
  OptionType type = OptionType::String;
  /// Value used when the caller does not set the option. Its JSON type must
  /// match `type` (integer, number, boolean, string, string).
  Json default_value;
  /// Inclusive bounds for Int and Float options; nullopt = unbounded.
  std::optional<double> min;
  std::optional<double> max;
  /// Allowed values of an Enum option, in display order.
  std::vector<std::string> choices;
  /// One sentence explaining the effect, shown as help text.
  std::string description;
  /// Whether the option belongs to the short list or the "advanced" section.
  OptionCategory category = OptionCategory::Basic;

  /// The same spec marked as advanced: `OptionSpec::integer(...).advanced()`.
  [[nodiscard]] OptionSpec advanced() const {
    OptionSpec copy = *this;
    copy.category = OptionCategory::Advanced;
    return copy;
  }

  [[nodiscard]] static OptionSpec integer(std::string key, std::string label, std::int64_t default_value,
                                          std::int64_t min, std::int64_t max, std::string description = {});
  [[nodiscard]] static OptionSpec number(std::string key, std::string label, double default_value, double min,
                                         double max, std::string description = {});
  [[nodiscard]] static OptionSpec boolean(std::string key, std::string label, bool default_value,
                                          std::string description = {});
  [[nodiscard]] static OptionSpec choice(std::string key, std::string label, std::string default_value,
                                         std::vector<std::string> choices, std::string description = {});
  [[nodiscard]] static OptionSpec text(std::string key, std::string label, std::string default_value,
                                       std::string description = {});
};

/// The JSON array published by `cvt_options_schema_json`:
/// `[{"key","label","type","default","min","max","choices":[],"description","category"}]`.
[[nodiscard]] Json option_schema_json(std::span<const OptionSpec> specs);

/// Option values for one conversion: a flat JSON object plus lenient typed
/// getters. Getters never fail: a missing or unusable value yields the
/// fallback, and the ranged overloads clamp.
///
/// Codecs normally just read what they declared - the engine has already
/// applied `normalized()` with the codec's specs, so every declared key is
/// present, of the right type and in range:
///
///   const auto width = ctx.options.get_int("line_width", 76);
class Options {
 public:
  /// No options set.
  Options();
  /// Wraps a JSON object; anything else is treated as empty.
  explicit Options(Json values);

  /// Parse the `options_json` argument of the C API. Empty or blank text
  /// means "no options". Fails with Status::InvalidArgument unless the text
  /// is a JSON object.
  [[nodiscard]] static Result<Options> parse(std::string_view json_text);

  /// A copy with `specs` applied: declared keys are coerced to their type
  /// ("80" -> 80, 1 -> true, ...), numbers are clamped to [min, max], and
  /// missing ones are filled with their default. Keys without a spec pass
  /// through untouched. Fails with Status::InvalidArgument when a value
  /// cannot be coerced or is not one of an enum's choices.
  [[nodiscard]] Result<Options> normalized(std::span<const OptionSpec> specs) const;

  [[nodiscard]] bool contains(std::string_view key) const;
  /// True when the caller supplied the value, false when it is absent or was
  /// filled in from an OptionSpec default by `normalized()`. Lets a codec
  /// tell "quality left alone" from "quality explicitly set to the default".
  [[nodiscard]] bool was_set(std::string_view key) const;
  [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

  [[nodiscard]] std::int64_t get_int(std::string_view key, std::int64_t fallback) const;
  /// Clamped to [min, max] (the fallback too).
  [[nodiscard]] std::int64_t get_int(std::string_view key, std::int64_t fallback, std::int64_t min,
                                     std::int64_t max) const;
  [[nodiscard]] double get_float(std::string_view key, double fallback) const;
  /// Clamped to [min, max]; NaN yields the fallback.
  [[nodiscard]] double get_float(std::string_view key, double fallback, double min, double max) const;
  [[nodiscard]] bool get_bool(std::string_view key, bool fallback) const;
  [[nodiscard]] std::string get_string(std::string_view key, std::string_view fallback = {}) const;
  /// The value if it is one of `choices` (ASCII case-insensitive; the
  /// canonical spelling from `choices` is returned), else the fallback.
  [[nodiscard]] std::string get_enum(std::string_view key, std::string_view fallback,
                                     std::initializer_list<std::string_view> choices) const;
  [[nodiscard]] std::string get_enum(std::string_view key, std::string_view fallback,
                                     std::span<const std::string> choices) const;

  /// Set or replace a value.
  void set(std::string_view key, Json value);
  /// Remove a value if present.
  void erase(std::string_view key);
  /// The underlying JSON object.
  [[nodiscard]] const Json& json() const noexcept { return values_; }

 private:
  [[nodiscard]] const Json* find(std::string_view key) const;
  Json values_;
  std::vector<std::string> defaulted_;  // keys whose value came from a spec default
};

}  // namespace cvt
