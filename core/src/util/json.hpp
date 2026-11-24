// Convertible core - JSON values, strict/lenient parser and serialiser.
//
// `cvt::Json` is the one JSON value type of the core: engine options, the
// JSON published by the C API, `ir::Data` and every JSON-shaped format family
// use it. It is written for this project and depends on nothing but the
// standard library.
//
//   Json doc = Json::object({{"name", "Ada"}, {"tags", Json::array({1, 2.5, nullptr})}});
//   doc["age"] = 36;                                   // inserts or replaces
//   for (const auto& [key, value] : doc.items()) ...   // insertion order
//   const std::string& name = doc["name"].as_string(); // "" when absent or not a string
//   CVT_TRY_ASSIGN(Json parsed, util::parse_json(text));
//   std::string text = util::dump_json(doc, 2);
//
// Design rules
//   - No member throws (std::bad_alloc aside) and none has a precondition
//     that can crash: reading through the wrong type yields a neutral value
//     (a shared null, an empty string, the fallback you passed).
//   - Objects keep insertion order and have unique keys.
//   - Arbitrarily deep values are safe: destruction, copying, comparison,
//     parsing and serialisation are iterative, never recursive.
//   - Const access to one value from several threads is safe; mutation needs
//     external synchronisation, as for standard containers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "engine/errors.hpp"

namespace cvt {

namespace detail {

/// Integer types that become JSON numbers: everything integral except `bool`
/// and the character types (a `char` is text, not a number).
template <class T>
concept JsonInteger = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char> &&
                      !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> && !std::same_as<T, char16_t> &&
                      !std::same_as<T, char32_t>;

}  // namespace detail

class Json;

namespace detail {

/// Ranges that become JSON arrays: their elements convert to Json, directly
/// or (for proxy references such as those of `std::vector<bool>`) through
/// their value type. Strings are not ranges here, and neither are types that
/// are ranges of themselves (`std::filesystem::path`).
template <class R>
concept JsonRange =
    std::ranges::input_range<R> && !std::convertible_to<R, std::string_view> &&
    !std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<R>>, std::remove_cvref_t<R>> &&
    (std::constructible_from<Json, std::ranges::range_reference_t<R>> ||
     (std::convertible_to<std::ranges::range_reference_t<R>, std::ranges::range_value_t<R>> &&
      std::constructible_from<Json, std::ranges::range_value_t<R>>));

}  // namespace detail

/// A JSON value: null, boolean, number, string, array or object.
///
/// Numbers come in three kinds - signed 64-bit integer, unsigned 64-bit
/// integer and double - so that every 64-bit integer survives a round trip.
/// The kind is visible through `type()`, but comparison and the numeric
/// getters work on the mathematical value, so it rarely matters.
///
/// Strings are byte strings that are expected to hold UTF-8. Parsed values
/// always do; values built in code are not checked, and the serialiser
/// replaces malformed sequences with U+FFFD.
///
/// `sizeof(Json)` is 16 bytes. Strings, arrays and objects live on the heap;
/// a moved-from value is null.
class Json {
 public:
  enum class Type : std::uint8_t { Null, Bool, Int64, UInt64, Double, String, Array, Object };

  /// One key/value pair of an object; see `items()`.
  class Member;

  // ---- construction ---------------------------------------------------------

  /// null.
  constexpr Json() noexcept = default;
  constexpr Json(std::nullptr_t) noexcept {}
  /// Exactly `bool`: pointers and other types never convert to a boolean.
  template <std::same_as<bool> T>
  constexpr Json(T value) noexcept : value_(value), type_(Type::Bool) {}
  /// Signed integer types become Int64, unsigned ones UInt64.
  template <detail::JsonInteger T>
    requires std::signed_integral<T>
  constexpr Json(T value) noexcept : value_(static_cast<std::int64_t>(value)), type_(Type::Int64) {}
  template <detail::JsonInteger T>
    requires std::unsigned_integral<T>
  constexpr Json(T value) noexcept : value_(static_cast<std::uint64_t>(value)), type_(Type::UInt64) {}
  /// Any floating-point type becomes Double. NaN and the infinities are
  /// representable in a value although JSON text has no spelling for them.
  template <std::floating_point T>
  constexpr Json(T value) noexcept : value_(static_cast<double>(value)), type_(Type::Double) {}
  Json(std::string value);
  Json(std::string_view value);
  /// A null pointer gives null.
  Json(const char* value);
  /// Any range whose elements convert to Json becomes an array:
  /// `Json(std::vector<std::string>{...})`, `Json(std::span<const int>(...))`.
  template <class R>
    requires detail::JsonRange<const R&>
  Json(const R& range) : Json(array()) {
    if constexpr (std::ranges::sized_range<const R&>) reserve(static_cast<std::size_t>(std::ranges::size(range)));
    for (auto&& element : range) {
      if constexpr (std::constructible_from<Json, std::ranges::range_reference_t<const R&>>) {
        push_back(Json(std::forward<decltype(element)>(element)));
      } else {
        push_back(Json(static_cast<std::ranges::range_value_t<const R&>>(element)));  // proxy references (vector<bool>)
      }
    }
  }

  /// An empty array / an array of the given elements: `Json::array({1, "two", nullptr})`.
  [[nodiscard]] static Json array();
  [[nodiscard]] static Json array(std::initializer_list<Json> elements);
  /// An empty object / an object of the given members, in the given order:
  /// `Json::object({{"id", 7}, {"tags", Json::array()}})`. A repeated key
  /// keeps its first position and takes the last value.
  [[nodiscard]] static Json object();
  [[nodiscard]] static Json object(std::initializer_list<std::pair<std::string_view, Json>> members);

  /// Deep copy. Cost is linear in the size of the value.
  Json(const Json& other);
  constexpr Json(Json&& other) noexcept : value_(other.value_), type_(other.type_) { other.type_ = Type::Null; }
  Json& operator=(const Json& other) {
    Json copy(other);
    swap(copy);
    return *this;
  }
  /// Safe for self-assignment and for assigning a value from one of its own
  /// children (`doc = std::move(doc["inner"])`).
  Json& operator=(Json&& other) noexcept {
    Json taken(std::move(other));
    swap(taken);
    return *this;
  }
  ~Json() {
    if (type_ >= Type::String) destroy();
  }

  void swap(Json& other) noexcept {
    std::swap(value_, other.value_);
    std::swap(type_, other.type_);
  }
  friend void swap(Json& a, Json& b) noexcept { a.swap(b); }

  // ---- type -----------------------------------------------------------------

  [[nodiscard]] constexpr Type type() const noexcept { return type_; }
  /// "null", "boolean", "number", "string", "array" or "object".
  [[nodiscard]] std::string_view type_name() const noexcept;

  [[nodiscard]] constexpr bool is_null() const noexcept { return type_ == Type::Null; }
  [[nodiscard]] constexpr bool is_bool() const noexcept { return type_ == Type::Bool; }
  /// Any of the three number kinds.
  [[nodiscard]] constexpr bool is_number() const noexcept { return type_ >= Type::Int64 && type_ <= Type::Double; }
  /// Stored as an integer (Int64 or UInt64). A Double is never an "integer"
  /// here, even when its value is whole; use `get_int64()` to accept those.
  [[nodiscard]] constexpr bool is_integer() const noexcept { return type_ == Type::Int64 || type_ == Type::UInt64; }
  /// Stored as UInt64. The parser uses UInt64 only for integers above
  /// INT64_MAX; values constructed from unsigned C++ types are UInt64 too.
  [[nodiscard]] constexpr bool is_unsigned() const noexcept { return type_ == Type::UInt64; }
  [[nodiscard]] constexpr bool is_float() const noexcept { return type_ == Type::Double; }
  [[nodiscard]] constexpr bool is_string() const noexcept { return type_ == Type::String; }
  [[nodiscard]] constexpr bool is_array() const noexcept { return type_ == Type::Array; }
  [[nodiscard]] constexpr bool is_object() const noexcept { return type_ == Type::Object; }
  /// Array or object.
  [[nodiscard]] constexpr bool is_structured() const noexcept { return type_ >= Type::Array; }

  // ---- scalar access ----------------------------------------------------------
  //
  // `get_*` return nullopt, and `as_*` the fallback, when the value has
  // another type or the conversion is not allowed. Nothing is coerced across
  // types: a string "1" is not a number and 1 is not `true`.

  [[nodiscard]] std::optional<bool> get_bool() const noexcept;
  /// Any number whose value is a whole number in the int64 range (so 3.0 and
  /// UInt64 7 qualify, 3.5 and 2^63 do not).
  [[nodiscard]] std::optional<std::int64_t> get_int64() const noexcept;
  /// Any number whose value is a whole number in the uint64 range.
  [[nodiscard]] std::optional<std::uint64_t> get_uint64() const noexcept;
  /// Any number. Integers beyond 2^53 are rounded to the nearest double.
  [[nodiscard]] std::optional<double> get_double() const noexcept;
  /// The view is valid until the value is modified or destroyed.
  [[nodiscard]] std::optional<std::string_view> get_string() const noexcept;

  [[nodiscard]] bool as_bool(bool fallback = false) const noexcept { return get_bool().value_or(fallback); }
  [[nodiscard]] std::int64_t as_int64(std::int64_t fallback = 0) const noexcept { return get_int64().value_or(fallback); }
  [[nodiscard]] std::uint64_t as_uint64(std::uint64_t fallback = 0) const noexcept {
    return get_uint64().value_or(fallback);
  }
  [[nodiscard]] double as_double(double fallback = 0.0) const noexcept { return get_double().value_or(fallback); }
  /// The string, or a shared empty string when the value is not a string.
  [[nodiscard]] const std::string& as_string() const noexcept;

  // ---- containers ---------------------------------------------------------------
  //
  // Reading never fails: a missing element, or asking an array question of a
  // non-array, yields a shared null / nullptr / an empty range.
  //
  // The mutators `push_back`, non-const `operator[]`, `set` and `insert` first
  // turn a value of any other type into an empty container of the kind they
  // need (so they work on a fresh null), discarding what it held.

  /// Number of elements (array) or members (object); 0 for everything else.
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  /// Capacity hint for an array or object; no effect on other types.
  void reserve(std::size_t count);

  /// The elements of an array (empty for other types):
  /// `for (const Json& element : value.elements())`.
  [[nodiscard]] std::span<const Json> elements() const noexcept;
  [[nodiscard]] std::span<Json> elements() noexcept;
  /// The element, or nullptr when out of range or not an array.
  [[nodiscard]] const Json* at(std::size_t index) const noexcept;
  [[nodiscard]] Json* at(std::size_t index) noexcept;
  /// The element, or a shared null when out of range or not an array.
  [[nodiscard]] const Json& operator[](std::size_t index) const noexcept;
  /// The element; the array is first extended with nulls up to `index`.
  [[nodiscard]] Json& operator[](std::size_t index);
  /// Appends, returning the new element.
  Json& push_back(Json value);
  /// Removes one element, shifting the rest down (linear). False when out of
  /// range or not an array.
  bool erase(std::size_t index) noexcept;

  /// The members of an object in insertion order (empty for other types):
  /// `for (const auto& [key, value] : value.items())`; `member.key()` and
  /// `member.value()` work too. Through the non-const overload values can be
  /// modified in place; keys cannot.
  [[nodiscard]] std::span<const Member> items() const noexcept;
  [[nodiscard]] std::span<Member> items() noexcept;
  /// The member's value, or nullptr when absent or not an object. Lookup
  /// compares keys directly for up to 16 members and uses a hash index
  /// (expected constant time, keyed per process against crafted collisions)
  /// beyond that.
  [[nodiscard]] const Json* find(std::string_view key) const noexcept;
  [[nodiscard]] Json* find(std::string_view key) noexcept;
  [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }
  /// The member's value, or a shared null when absent or not an object.
  [[nodiscard]] const Json& operator[](std::string_view key) const noexcept;
  /// The member's value; a missing member is appended as null first.
  [[nodiscard]] Json& operator[](std::string_view key);
  /// Insert-or-replace: an existing key keeps its position and gets the new
  /// value, a new key is appended. Returns the stored value.
  Json& set(std::string_view key, Json value);
  /// Appends the member only when the key is absent; returns whether it did.
  bool insert(std::string_view key, Json value);
  /// Removes a member, keeping the order of the rest (linear). False when
  /// absent or not an object.
  bool erase(std::string_view key) noexcept;

  // ---- comparison -------------------------------------------------------------

  /// Structural equality.
  ///   - Numbers compare by mathematical value across the three kinds:
  ///     1 == 1.0, UInt64 5 == Int64 5, and 2^53 + 1 != 2^53 even though a
  ///     double cannot tell them apart. NaN equals nothing, itself included;
  ///     0.0 == -0.0.
  ///   - Objects are equal when they have the same keys with equal values;
  ///     member order is presentation and is ignored (see `equal_ordered`).
  ///   - Arrays are equal when they have equal elements in the same order.
  ///   - Values of different types are never equal ("1" != 1, null != false).
  [[nodiscard]] friend bool operator==(const Json& a, const Json& b) { return equal(a, b, false); }
  /// As `==`, but objects must also list their members in the same order.
  [[nodiscard]] bool equal_ordered(const Json& other) const { return equal(*this, other, true); }

 private:
  struct ArrayNode;
  struct ObjectNode;

  union Value {
    std::uint64_t uint64;
    std::int64_t int64;
    double float64;
    bool boolean;
    std::string* string;
    ArrayNode* array;
    ObjectNode* object;

    constexpr Value() noexcept : uint64(0) {}
    constexpr explicit Value(std::uint64_t value) noexcept : uint64(value) {}
    constexpr explicit Value(std::int64_t value) noexcept : int64(value) {}
    constexpr explicit Value(double value) noexcept : float64(value) {}
    constexpr explicit Value(bool value) noexcept : boolean(value) {}
  };

  void destroy() noexcept;
  void become(Type type);
  [[nodiscard]] static bool equal(const Json& a, const Json& b, bool ordered);

  Value value_;
  Type type_ = Type::Null;
};

class Json::Member {
 public:
  Member(std::string key, Json value) : key_(std::move(key)), value_(std::move(value)) {}
  Member(const Member&) = default;
  Member(Member&&) noexcept = default;

  [[nodiscard]] const std::string& key() const noexcept { return key_; }
  [[nodiscard]] const Json& value() const noexcept { return value_; }
  [[nodiscard]] Json& value() noexcept { return value_; }

  /// Structured-binding support: `auto& [key, value] = member;`.
  template <std::size_t I>
    requires(I < 2)
  [[nodiscard]] decltype(auto) get() const noexcept {
    if constexpr (I == 0) {
      return (key_);
    } else {
      return (value_);
    }
  }
  template <std::size_t I>
    requires(I < 2)
  [[nodiscard]] decltype(auto) get() noexcept {
    if constexpr (I == 0) {
      return static_cast<const std::string&>(key_);
    } else {
      return (value_);
    }
  }

 private:
  friend class Json;
  // Whole members are never assigned from outside: a key must not change
  // behind the object's index.
  Member& operator=(const Member&) = default;
  Member& operator=(Member&&) noexcept = default;

  std::string key_;
  Json value_;
};

static_assert(sizeof(Json) <= 16, "Json is meant to stay two words on 64-bit targets");
static_assert(std::is_nothrow_move_constructible_v<Json> && std::is_nothrow_move_assignable_v<Json>);
static_assert(std::is_nothrow_default_constructible_v<Json>);

namespace util {

/// Maximum nesting depth `parse_json` accepts by default. Deeper documents
/// are rejected so that recursive consumers cannot overflow the stack.
inline constexpr std::size_t default_json_depth = 512;

/// What the parser does when an object repeats a key.
enum class JsonDuplicateKeys {
  LastWins,   ///< the key keeps its first position and takes the last value
  FirstWins,  ///< later occurrences are parsed and dropped
  Reject,     ///< Status::Decode, "duplicate key"
};

/// Parser settings. The defaults are strict RFC 8259; each `allow_*` switch
/// admits one widely used extension so that JSONC/JSON5-style readers can
/// reuse the parser. (This is not a complete JSON5 implementation: hex
/// numbers, a leading '+', bare '.5' and line continuations are not accepted.)
struct JsonParseOptions {
  /// Maximum number of arrays/objects open at once. 0 admits only scalars.
  std::size_t max_depth = default_json_depth;
  /// `// to end of line` and `/* block */` comments wherever whitespace may appear.
  bool allow_comments = false;
  /// One comma after the last element or member: `[1, 2,]`, `{"a": 1,}`.
  bool allow_trailing_commas = false;
  /// The literals `NaN`, `Infinity` and `-Infinity` (as doubles).
  bool allow_nan_inf = false;
  /// 'single-quoted' strings and keys, with `\'` as an extra escape.
  bool allow_single_quotes = false;
  /// Unquoted object keys made of ASCII letters, digits (not first), '_',
  /// '$' and any non-ASCII character: `{name: 1}`.
  bool allow_unquoted_keys = false;
  /// Raw control characters (U+0000..U+001F) inside strings, kept as they are.
  bool allow_control_characters = false;
  /// Malformed UTF-8 inside strings and comments, and `\u` escapes that are
  /// lone surrogates, become U+FFFD instead of errors.
  bool replace_invalid_utf8 = false;
  JsonDuplicateKeys duplicate_keys = JsonDuplicateKeys::LastWins;
};

/// Where a parse error was detected.
struct JsonLocation {
  std::size_t offset = 0;  ///< byte offset into the text as passed in (a BOM counts)
  std::size_t line = 1;    ///< 1-based; LF, CRLF and lone CR each end a line
  std::size_t column = 1;  ///< 1-based, in code points (a leading BOM is not counted)
};

/// Parse UTF-8 JSON text strictly per RFC 8259 (a leading byte order mark is
/// tolerated). Never throws: syntax errors, invalid UTF-8, out-of-range
/// numbers and excessive nesting come back as Status::Decode with a message
/// of the form
///   `invalid JSON at line 3, column 14 (byte 57): expected ',' or '}'`.
///
/// Numbers: an integer literal (no fraction or exponent) becomes Int64 when
/// it fits, else UInt64 when it fits, else a double; everything else becomes
/// the correctly rounded double. `-0` is the integer 0, `-0.0` the double
/// -0.0. A literal too large for a double (1e999) is an error; one too small
/// (1e-999) is 0.0. A repeated object key keeps its first position and takes
/// the last value.
///
/// The parser is iterative and single-pass: time and memory are linear in the
/// input, whatever its shape.
[[nodiscard]] Result<Json> parse_json(std::string_view text, std::size_t max_depth = default_json_depth);

/// As above with explicit settings. `error_location`, when given, receives
/// the position of the error on failure (and is left untouched on success).
[[nodiscard]] Result<Json> parse_json(std::string_view text, const JsonParseOptions& options,
                                      JsonLocation* error_location = nullptr);

/// Serialiser settings.
struct JsonWriteOptions {
  /// < 0: compact, no whitespace at all (`{"a":1,"b":[true,null]}`).
  /// >= 0: one element/member per line, indented by that many spaces per
  /// level, `"key": value`, empty containers as `[]` / `{}`.
  int indent = -1;
  /// Write every character above U+007E as `\uXXXX` (surrogate pairs beyond
  /// the BMP), so the output is pure ASCII.
  bool ascii_only = false;
  /// Write '/' as `\/` (for embedding in HTML script elements).
  bool escape_slash = false;
  /// Write object members sorted by key (byte order, which for UTF-8 is code
  /// point order) instead of insertion order.
  bool sort_keys = false;
  /// Line terminator of the indented form.
  std::string_view newline = "\n";
  /// Write non-finite doubles as `NaN`, `Infinity` and `-Infinity` (not
  /// JSON; readable with `allow_nan_inf`) instead of `null`.
  bool nan_inf_as_literals = false;
};

/// Serialise to UTF-8. `indent < 0` gives the compact single-line form,
/// otherwise that many spaces per level. Never throws: invalid UTF-8 inside
/// strings is replaced with U+FFFD, and non-finite doubles are written as
/// `null`, so the output is always valid JSON in valid UTF-8.
///
/// Numbers: integers are written exactly. A double is written with the
/// fewest digits that read back as the identical double, in positional
/// notation when 1e-4 <= |x| < 1e16 and in exponent notation otherwise, and
/// always with a '.' or an 'e' so that it stays a double when parsed again:
/// 1.0, -0.0, 0.1, 1234.5, 1e+16, 1.5e-07, 5e-324.
///
/// Serialisation is iterative; the indented form of a value nested d levels
/// deep needs up to d * indent characters of indentation per line.
[[nodiscard]] std::string dump_json(const Json& value, int indent = -1);
[[nodiscard]] std::string dump_json(const Json& value, const JsonWriteOptions& options);

}  // namespace util
}  // namespace cvt

template <>
struct std::tuple_size<cvt::Json::Member> : std::integral_constant<std::size_t, 2> {};
template <>
struct std::tuple_element<0, cvt::Json::Member> {
  using type = const std::string;
};
template <>
struct std::tuple_element<1, cvt::Json::Member> {
  using type = cvt::Json;
};
