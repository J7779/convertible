// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/options.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/text.hpp"

namespace cvt {

namespace {

std::optional<double> to_number(const Json& value) {
  if (value.is_number()) return value.as_double();
  if (value.is_bool()) return value.as_bool() ? 1.0 : 0.0;
  if (value.is_string()) return util::parse_double(util::trim(value.as_string()));
  return std::nullopt;
}

std::optional<std::int64_t> to_integer(const Json& value) {
  if (value.is_integer()) {
    // An unsigned value beyond the int64 range saturates.
    return value.as_int64(std::numeric_limits<std::int64_t>::max());
  }
  if (value.is_string()) {
    if (const auto parsed = util::parse_int(util::trim(value.as_string()))) return parsed;
  }
  const auto number = to_number(value);
  if (!number || std::isnan(*number)) return std::nullopt;
  const double rounded = std::round(*number);
  // 2^63 is exactly representable; anything at or beyond it saturates.
  if (rounded >= 9223372036854775808.0) return std::numeric_limits<std::int64_t>::max();
  if (rounded <= -9223372036854775808.0) return std::numeric_limits<std::int64_t>::min();
  return static_cast<std::int64_t>(rounded);
}

std::optional<bool> to_bool(const Json& value) {
  if (value.is_bool()) return value.as_bool();
  if (value.is_number()) return value.as_double() != 0.0;
  if (value.is_string()) {
    const std::string_view text = util::trim(value.as_string());
    for (const std::string_view yes : {"true", "1", "yes", "on"}) {
      if (util::iequals(text, yes)) return true;
    }
    for (const std::string_view no : {"false", "0", "no", "off"}) {
      if (util::iequals(text, no)) return false;
    }
  }
  return std::nullopt;
}

std::optional<std::string> to_text(const Json& value) {
  if (value.is_string()) return value.as_string();
  if (value.is_bool()) return std::string(value.as_bool() ? "true" : "false");
  if (value.is_integer()) return util::dump_json(value);
  if (value.is_number()) return util::format_double(value.as_double());
  return std::nullopt;
}

template <class Choices>
std::optional<std::string> match_choice(std::string_view value, const Choices& choices) {
  for (const auto& choice : choices) {
    if (util::iequals(value, choice)) return std::string(choice);
  }
  return std::nullopt;
}

}  // namespace

OptionSpec OptionSpec::integer(std::string key, std::string label, std::int64_t default_value, std::int64_t min,
                               std::int64_t max, std::string description) {
  OptionSpec spec;
  spec.key = std::move(key);
  spec.label = std::move(label);
  spec.type = OptionType::Int;
  spec.default_value = default_value;
  spec.min = static_cast<double>(min);
  spec.max = static_cast<double>(max);
  spec.description = std::move(description);
  return spec;
}

OptionSpec OptionSpec::number(std::string key, std::string label, double default_value, double min, double max,
                              std::string description) {
  OptionSpec spec;
  spec.key = std::move(key);
  spec.label = std::move(label);
  spec.type = OptionType::Float;
  spec.default_value = default_value;
  spec.min = min;
  spec.max = max;
  spec.description = std::move(description);
  return spec;
}

OptionSpec OptionSpec::boolean(std::string key, std::string label, bool default_value, std::string description) {
  OptionSpec spec;
  spec.key = std::move(key);
  spec.label = std::move(label);
  spec.type = OptionType::Bool;
  spec.default_value = default_value;
  spec.description = std::move(description);
  return spec;
}

OptionSpec OptionSpec::choice(std::string key, std::string label, std::string default_value,
                              std::vector<std::string> choices, std::string description) {
  OptionSpec spec;
  spec.key = std::move(key);
  spec.label = std::move(label);
  spec.type = OptionType::Enum;
  spec.default_value = std::move(default_value);
  spec.choices = std::move(choices);
  spec.description = std::move(description);
  return spec;
}

OptionSpec OptionSpec::text(std::string key, std::string label, std::string default_value, std::string description) {
  OptionSpec spec;
  spec.key = std::move(key);
  spec.label = std::move(label);
  spec.type = OptionType::String;
  spec.default_value = std::move(default_value);
  spec.description = std::move(description);
  return spec;
}

Json option_schema_json(std::span<const OptionSpec> specs) {
  Json out = Json::array();
  for (const OptionSpec& spec : specs) {
    Json entry = Json::object();
    entry["key"] = spec.key;
    entry["label"] = spec.label;
    entry["type"] = option_type_name(spec.type);
    entry["default"] = spec.default_value;
    const auto bound = [&](const std::optional<double>& value) -> Json {
      if (!value) return nullptr;
      if (spec.type == OptionType::Int) return static_cast<std::int64_t>(*value);
      return *value;
    };
    entry["min"] = bound(spec.min);
    entry["max"] = bound(spec.max);
    entry["choices"] = spec.choices;
    entry["description"] = spec.description;
    entry["category"] = option_category_name(spec.category);
    out.push_back(std::move(entry));
  }
  return out;
}

Options::Options() : values_(Json::object()) {}

Options::Options(Json values) : values_(values.is_object() ? std::move(values) : Json::object()) {}

Result<Options> Options::parse(std::string_view json_text) {
  if (util::trim(json_text).empty()) return Options();
  auto parsed = util::parse_json(json_text, 64);
  if (!parsed) return fail(Status::InvalidArgument, "options are not valid JSON: {}", parsed.error().message);
  if (parsed->is_null()) return Options();
  if (!parsed->is_object()) return fail(Status::InvalidArgument, "options must be a JSON object");
  return Options(std::move(*parsed));
}

Result<Options> Options::normalized(std::span<const OptionSpec> specs) const {
  Options out = *this;
  for (const OptionSpec& spec : specs) {
    const Json* given = find(spec.key);
    if (given == nullptr || given->is_null()) {
      out.values_[spec.key] = spec.default_value;
      if (std::ranges::find(out.defaulted_, spec.key) == out.defaulted_.end()) out.defaulted_.push_back(spec.key);
      continue;
    }
    const auto bad = [&](std::string_view expected) {
      return fail(Status::InvalidArgument, "option '{}' must be {}", spec.key, expected);
    };
    switch (spec.type) {
      case OptionType::Int: {
        const auto value = to_integer(*given);
        if (!value) return bad("a whole number");
        std::int64_t clamped = *value;
        if (spec.min && static_cast<double>(clamped) < *spec.min) clamped = static_cast<std::int64_t>(*spec.min);
        if (spec.max && static_cast<double>(clamped) > *spec.max) clamped = static_cast<std::int64_t>(*spec.max);
        out.values_[spec.key] = clamped;
        break;
      }
      case OptionType::Float: {
        const auto value = to_number(*given);
        if (!value || std::isnan(*value)) return bad("a number");
        double clamped = *value;
        if (spec.min) clamped = std::max(clamped, *spec.min);
        if (spec.max) clamped = std::min(clamped, *spec.max);
        out.values_[spec.key] = clamped;
        break;
      }
      case OptionType::Bool: {
        const auto value = to_bool(*given);
        if (!value) return bad("true or false");
        out.values_[spec.key] = *value;
        break;
      }
      case OptionType::Enum: {
        const auto text = to_text(*given);
        const auto match = text ? match_choice(*text, spec.choices) : std::nullopt;
        if (!match) return bad("one of: " + util::join(spec.choices, ", "));
        out.values_[spec.key] = *match;
        break;
      }
      case OptionType::String: {
        const auto text = to_text(*given);
        if (!text) return bad("a string");
        out.values_[spec.key] = *text;
        break;
      }
    }
  }
  return out;
}

const Json* Options::find(std::string_view key) const {
  return values_.find(key);
}

bool Options::contains(std::string_view key) const {
  const Json* value = find(key);
  return value != nullptr && !value->is_null();
}

bool Options::was_set(std::string_view key) const {
  return contains(key) && std::ranges::find(defaulted_, key) == defaulted_.end();
}

std::int64_t Options::get_int(std::string_view key, std::int64_t fallback) const {
  const Json* value = find(key);
  if (value == nullptr) return fallback;
  return to_integer(*value).value_or(fallback);
}

std::int64_t Options::get_int(std::string_view key, std::int64_t fallback, std::int64_t min, std::int64_t max) const {
  if (min > max) std::swap(min, max);
  return std::clamp(get_int(key, fallback), min, max);
}

double Options::get_float(std::string_view key, double fallback) const {
  const Json* value = find(key);
  if (value == nullptr) return fallback;
  const auto number = to_number(*value);
  return number && !std::isnan(*number) ? *number : fallback;
}

double Options::get_float(std::string_view key, double fallback, double min, double max) const {
  if (min > max) std::swap(min, max);
  return std::clamp(get_float(key, fallback), min, max);
}

bool Options::get_bool(std::string_view key, bool fallback) const {
  const Json* value = find(key);
  if (value == nullptr) return fallback;
  return to_bool(*value).value_or(fallback);
}

std::string Options::get_string(std::string_view key, std::string_view fallback) const {
  const Json* value = find(key);
  if (value == nullptr) return std::string(fallback);
  return to_text(*value).value_or(std::string(fallback));
}

std::string Options::get_enum(std::string_view key, std::string_view fallback,
                              std::initializer_list<std::string_view> choices) const {
  const Json* value = find(key);
  if (value == nullptr) return std::string(fallback);
  const auto text = to_text(*value);
  if (!text) return std::string(fallback);
  return match_choice(*text, choices).value_or(std::string(fallback));
}

std::string Options::get_enum(std::string_view key, std::string_view fallback,
                              std::span<const std::string> choices) const {
  const Json* value = find(key);
  if (value == nullptr) return std::string(fallback);
  const auto text = to_text(*value);
  if (!text) return std::string(fallback);
  return match_choice(*text, choices).value_or(std::string(fallback));
}

void Options::set(std::string_view key, Json value) {
  std::erase(defaulted_, key);
  values_.set(key, std::move(value));
}

void Options::erase(std::string_view key) {
  std::erase(defaulted_, key);
  values_.erase(key);
}

}  // namespace cvt
