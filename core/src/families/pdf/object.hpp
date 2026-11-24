// Convertible core - pdf family: the PDF object model.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::pdf {

/// An indirect reference "num gen R".
struct Ref {
  std::uint32_t num = 0;
  std::uint32_t gen = 0;
  friend bool operator==(const Ref&, const Ref&) = default;
  friend auto operator<=>(const Ref&, const Ref&) = default;
};

/// A PDF name, without the slash, with #xx escapes resolved.
struct Name {
  std::string value;
  friend bool operator==(const Name&, const Name&) = default;
};

/// A PDF string: raw bytes (literal escapes / hex resolved, decrypted).
struct String {
  std::string bytes;
  bool hex = false;  ///< written as <...> in the source (a hint for the writer)
};

class Object;
class Dict;
struct Stream;
using Array = std::vector<Object>;

/// One PDF value. Arrays, dictionaries and streams are shared (cheap copies);
/// treat them as immutable once they are reachable from more than one place.
class Object {
 public:
  enum class Type : std::uint8_t { Null, Bool, Int, Real, String, Name, Array, Dict, Stream, Ref };

  Object() = default;
  Object(std::nullptr_t) {}  // NOLINT(google-explicit-constructor)
  Object(bool v) : value_(v) {}  // NOLINT(google-explicit-constructor)
  Object(int v) : value_(static_cast<std::int64_t>(v)) {}  // NOLINT(google-explicit-constructor)
  Object(std::int64_t v) : value_(v) {}  // NOLINT(google-explicit-constructor)
  Object(double v) : value_(v) {}  // NOLINT(google-explicit-constructor)
  Object(String v) : value_(std::move(v)) {}  // NOLINT(google-explicit-constructor)
  Object(Name v) : value_(std::move(v)) {}  // NOLINT(google-explicit-constructor)
  Object(Array v);  // NOLINT(google-explicit-constructor)
  Object(Dict v);   // NOLINT(google-explicit-constructor)
  Object(Stream v);  // NOLINT(google-explicit-constructor)
  Object(std::shared_ptr<const Array> v) : value_(std::move(v)) {}  // NOLINT(google-explicit-constructor)
  Object(std::shared_ptr<const Dict> v) : value_(std::move(v)) {}  // NOLINT(google-explicit-constructor)
  Object(std::shared_ptr<const Stream> v) : value_(std::move(v)) {}  // NOLINT(google-explicit-constructor)
  Object(Ref v) : value_(v) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Object name(std::string_view n) { return Object(Name{std::string(n)}); }
  [[nodiscard]] static Object string(std::string_view s, bool hex = false) {
    return Object(String{std::string(s), hex});
  }

  [[nodiscard]] Type type() const noexcept { return static_cast<Type>(value_.index()); }
  [[nodiscard]] bool is_null() const noexcept { return type() == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type() == Type::Bool; }
  [[nodiscard]] bool is_int() const noexcept { return type() == Type::Int; }
  [[nodiscard]] bool is_number() const noexcept { return type() == Type::Int || type() == Type::Real; }
  [[nodiscard]] bool is_string() const noexcept { return type() == Type::String; }
  [[nodiscard]] bool is_name() const noexcept { return type() == Type::Name; }
  [[nodiscard]] bool is_name(std::string_view n) const noexcept {
    return type() == Type::Name && std::get<Name>(value_).value == n;
  }
  [[nodiscard]] bool is_array() const noexcept { return type() == Type::Array; }
  [[nodiscard]] bool is_dict() const noexcept { return type() == Type::Dict || type() == Type::Stream; }
  [[nodiscard]] bool is_stream() const noexcept { return type() == Type::Stream; }
  [[nodiscard]] bool is_ref() const noexcept { return type() == Type::Ref; }

  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept {
    return is_bool() ? std::get<bool>(value_) : fallback;
  }
  [[nodiscard]] std::int64_t as_int(std::int64_t fallback = 0) const noexcept;
  [[nodiscard]] double as_number(double fallback = 0.0) const noexcept;
  /// The name without slash; empty when not a name.
  [[nodiscard]] const std::string& as_name() const noexcept;
  /// String bytes; empty when not a string.
  [[nodiscard]] const std::string& as_string() const noexcept;
  [[nodiscard]] const String* string_object() const noexcept { return std::get_if<String>(&value_); }
  /// Empty array when not an array.
  [[nodiscard]] const Array& as_array() const noexcept;
  /// The dictionary of a dict or stream; empty dict otherwise.
  [[nodiscard]] const Dict& as_dict() const noexcept;
  [[nodiscard]] const Stream* as_stream() const noexcept;
  [[nodiscard]] std::shared_ptr<const Stream> stream_ptr() const noexcept;
  [[nodiscard]] std::shared_ptr<const Dict> dict_ptr() const noexcept;
  [[nodiscard]] std::shared_ptr<const Array> array_ptr() const noexcept;
  [[nodiscard]] Ref as_ref() const noexcept { return is_ref() ? std::get<Ref>(value_) : Ref{}; }

 private:
  std::variant<std::monostate, bool, std::int64_t, double, String, Name, std::shared_ptr<const Array>,
               std::shared_ptr<const Dict>, std::shared_ptr<const Stream>, Ref>
      value_;
};

/// A dictionary with insertion order kept (deterministic output).
class Dict {
 public:
  using Entry = std::pair<std::string, Object>;

  [[nodiscard]] const Object* find(std::string_view key) const noexcept {
    for (const Entry& e : entries_) {
      if (e.first == key) return &e.second;
    }
    return nullptr;
  }
  /// The value or a null object.
  [[nodiscard]] const Object& get(std::string_view key) const noexcept;
  [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }
  void set(std::string_view key, Object value) {
    for (Entry& e : entries_) {
      if (e.first == key) {
        e.second = std::move(value);
        return;
      }
    }
    entries_.emplace_back(std::string(key), std::move(value));
  }
  void erase(std::string_view key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->first == key) {
        entries_.erase(it);
        return;
      }
    }
  }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] auto end() const noexcept { return entries_.end(); }

 private:
  std::vector<Entry> entries_;
};

/// A stream: its dictionary and its data as stored in the file (still
/// encoded with its /Filter chain, but already DECRYPTED).
struct Stream {
  Dict dict;
  Bytes data;
};

inline Object::Object(Array v) : value_(std::make_shared<const Array>(std::move(v))) {}
inline Object::Object(Dict v) : value_(std::make_shared<const Dict>(std::move(v))) {}
inline Object::Object(Stream v) : value_(std::make_shared<const Stream>(std::move(v))) {}

/// Serialise an object in PDF syntax (no indirect object wrapper). Streams
/// are written as their dictionary only.
void serialize(const Object& object, std::string& out);
[[nodiscard]] std::string format_number(double value);

}  // namespace cvt::pdf
