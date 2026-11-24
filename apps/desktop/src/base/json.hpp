// Convertible desktop - a small JSON value type with a parser and a writer.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cvd {

class Json {
 public:
  using Array = std::vector<Json>;
  /// Objects keep their members in document order.
  using Object = std::vector<std::pair<std::string, Json>>;

  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool v) : value_(v) {}
  Json(int v) : value_(static_cast<double>(v)) {}
  Json(std::int64_t v) : value_(static_cast<double>(v)) {}
  Json(double v) : value_(v) {}
  Json(const char* v) : value_(std::string(v)) {}
  Json(std::string v) : value_(std::move(v)) {}
  Json(std::string_view v) : value_(std::string(v)) {}
  Json(Array v) : value_(std::make_shared<Array>(std::move(v))) {}
  Json(Object v) : value_(std::make_shared<Object>(std::move(v))) {}

  static Json array() { return Json(Array{}); }
  static Json object() { return Json(Object{}); }

  /// Parses a complete JSON document; nullopt on any syntax error.
  static std::optional<Json> parse(std::string_view text);

  [[nodiscard]] Type type() const;
  [[nodiscard]] bool is_null() const { return type() == Type::Null; }
  [[nodiscard]] bool is_bool() const { return type() == Type::Bool; }
  [[nodiscard]] bool is_number() const { return type() == Type::Number; }
  [[nodiscard]] bool is_string() const { return type() == Type::String; }
  [[nodiscard]] bool is_array() const { return type() == Type::Array; }
  [[nodiscard]] bool is_object() const { return type() == Type::Object; }

  [[nodiscard]] bool as_bool(bool fallback = false) const;
  [[nodiscard]] double as_number(double fallback = 0.0) const;
  [[nodiscard]] int as_int(int fallback = 0) const;
  [[nodiscard]] std::string as_string(std::string_view fallback = {}) const;
  [[nodiscard]] const std::string& str() const;  // "" when not a string

  [[nodiscard]] const Array& items() const;   // empty when not an array
  [[nodiscard]] const Object& members() const;  // empty when not an object
  [[nodiscard]] std::size_t size() const;

  /// Object member lookup; a shared null value when missing.
  [[nodiscard]] const Json& operator[](std::string_view key) const;
  /// Array element; a shared null value when out of range.
  [[nodiscard]] const Json& operator[](std::size_t index) const;
  [[nodiscard]] bool contains(std::string_view key) const;

  /// Mutation (copy-on-write for shared containers).
  void push_back(Json v);
  void set(std::string_view key, Json v);

  /// Serialises; indent < 0 writes compact JSON.
  [[nodiscard]] std::string dump(int indent = -1) const;

  bool operator==(const Json& other) const;

 private:
  void dump_to(std::string& out, int indent, int depth) const;
  Array& mutable_array();
  Object& mutable_object();

  std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> value_;
};

/// Escapes a string as a JSON string literal including the quotes.
std::string json_quote(std::string_view text);

}  // namespace cvd
