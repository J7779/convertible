// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "util/bytes.hpp"
#include "util/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <vector>

#include "util/text.hpp"

namespace cvt {

// ===========================================================================
// Object index
// ===========================================================================
//
// An object is a vector of members in insertion order. Up to
// `index_threshold` members a lookup simply compares keys; larger objects
// carry an open-addressing hash table of member positions on the side. The
// table is only an index - iteration order never depends on it - so the hash
// can be keyed with a per-process secret, which keeps crafted documents from
// degrading lookups to linear time.

namespace {

constexpr std::size_t index_threshold = 16;
constexpr std::size_t npos = static_cast<std::size_t>(-1);

struct Slot {
  std::uint32_t hash = 0;      ///< low 32 bits of the key's hash; the home slot is `hash & mask`
  std::uint32_t position = 0;  ///< member index + 1; 0 marks an empty slot
};

/// The table is sized 4x the member count when it grows and its slot numbers
/// must fit the 32 stored hash bits, which bounds the indexable size; a
/// larger object (tens of gigabytes) falls back to unindexed lookups.
constexpr std::size_t max_indexed_members =
    static_cast<std::size_t>(std::min<std::uint64_t>(std::uint64_t{1} << 30, std::numeric_limits<std::size_t>::max() / 64));

std::array<std::uint64_t, 2> make_hash_secret() noexcept {
  std::array<std::uint64_t, 2> secret{0x243F6A8885A308D3ull, 0x13198A2E03707344ull};
  try {
    std::random_device device;
    for (std::uint64_t& word : secret) {
      const std::uint64_t high = device();
      const std::uint64_t low = device();
      word ^= (high << 32) ^ low;
    }
  } catch (...) {
    // No entropy source: fall back to values an input file cannot know.
    secret[0] ^= static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    secret[1] ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&secret));
  }
  return secret;
}

/// SipHash-1-3 of the key under the process secret, folded to 32 bits.
std::uint32_t hash_key(std::string_view key) noexcept {
  static const std::array<std::uint64_t, 2> secret = make_hash_secret();
  std::uint64_t v0 = secret[0] ^ 0x736F6D6570736575ull;
  std::uint64_t v1 = secret[1] ^ 0x646F72616E646F6Dull;
  std::uint64_t v2 = secret[0] ^ 0x6C7967656E657261ull;
  std::uint64_t v3 = secret[1] ^ 0x7465646279746573ull;
  const auto round = [&]() noexcept {
    v0 += v1;
    v1 = std::rotl(v1, 13);
    v1 ^= v0;
    v0 = std::rotl(v0, 32);
    v2 += v3;
    v3 = std::rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = std::rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = std::rotl(v1, 17);
    v1 ^= v2;
    v2 = std::rotl(v2, 32);
  };
  const char* data = key.data();
  std::size_t left = key.size();
  for (; left >= 8; data += 8, left -= 8) {
    std::uint64_t word = 0;
    ::cvt::util::copy_bytes(&word, data, 8);
    v3 ^= word;
    round();
    v0 ^= word;
  }
  std::uint64_t last = static_cast<std::uint64_t>(key.size()) << 56;
  for (std::size_t i = 0; i < left; ++i) last |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i);
  v3 ^= last;
  round();
  v0 ^= last;
  v2 ^= 0xFF;
  round();
  round();
  round();
  const std::uint64_t hash = v0 ^ v1 ^ v2 ^ v3;
  return static_cast<std::uint32_t>(hash ^ (hash >> 32));
}

void place(std::vector<Slot>& table, Slot slot) noexcept {
  const std::size_t mask = table.size() - 1;
  std::size_t i = slot.hash & mask;
  while (table[i].position != 0) i = (i + 1) & mask;
  table[i] = slot;
}

const Json& shared_null() noexcept {
  static const Json value;
  return value;
}

}  // namespace

struct Json::ArrayNode {
  std::vector<Json> items;
  ArrayNode* doomed = nullptr;  ///< intrusive work list of `destroy()`
};

struct Json::ObjectNode {
  std::vector<Member> members;
  std::vector<Slot> slots;       ///< empty, or a power-of-two table at most half full (linear probing)
  ObjectNode* doomed = nullptr;  ///< intrusive work list of `destroy()`

  /// Index of the member with this key, or npos.
  [[nodiscard]] std::size_t locate(std::string_view key) const noexcept {
    if (!slots.empty()) return probe(key, hash_key(key));
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (members[i].key() == key) return i;
    }
    return npos;
  }

  /// The value stored under `key`, appended as null when absent (second:
  /// whether it was appended).
  [[nodiscard]] std::pair<Json*, bool> find_or_append(std::string_view key) {
    const std::size_t count = members.size() + 1;
    const bool hashed = !slots.empty() || (count > index_threshold && count <= max_indexed_members);
    const std::uint32_t hash = hashed ? hash_key(key) : 0;
    const std::size_t found = slots.empty() ? locate(key) : probe(key, hash);
    if (found != npos) return {&members[found].value(), false};

    // Bring the table to its new size before the member exists, so that a
    // failed allocation leaves the object unchanged and the index consistent.
    if (count > max_indexed_members) {
      slots.clear();
    } else if (count > index_threshold && slots.size() < count * 2) {
      std::vector<Slot> grown(std::bit_ceil(count * 4));
      if (slots.empty()) {
        for (std::size_t i = 0; i < members.size(); ++i) {
          place(grown, Slot{hash_key(members[i].key()), static_cast<std::uint32_t>(i + 1)});
        }
      } else {
        for (const Slot& slot : slots) {
          if (slot.position != 0) place(grown, slot);
        }
      }
      slots.swap(grown);
    }
    members.emplace_back(std::string(key), Json());
    if (!slots.empty()) place(slots, Slot{hash, static_cast<std::uint32_t>(count)});
    return {&members.back().value(), true};
  }

  /// Updates the table after the member at `position` was removed and the
  /// later ones moved down by one. Never allocates.
  void forget(std::size_t position) noexcept {
    if (slots.empty()) return;
    const auto removed = static_cast<std::uint32_t>(position + 1);
    std::size_t hole = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (slots[i].position == removed) {
        hole = i;
      } else if (slots[i].position > removed) {
        --slots[i].position;
      }
    }
    // Close the gap so that probe sequences stay unbroken: an entry further
    // along moves into the hole unless its home slot lies after the hole.
    const std::size_t mask = slots.size() - 1;
    for (std::size_t next = (hole + 1) & mask; slots[next].position != 0; next = (next + 1) & mask) {
      const std::size_t home = slots[next].hash & mask;
      if (((next - home) & mask) >= ((next - hole) & mask)) {
        slots[hole] = slots[next];
        hole = next;
      }
    }
    slots[hole] = Slot{};
  }

  /// Index of the member with this key and hash in a non-empty table, or npos.
  [[nodiscard]] std::size_t probe(std::string_view key, std::uint32_t hash) const noexcept {
    const std::size_t mask = slots.size() - 1;
    for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
      const Slot slot = slots[i];
      if (slot.position == 0) return npos;
      if (slot.hash == hash && members[slot.position - 1].key() == key) return slot.position - 1;
    }
  }
};

// ===========================================================================
// Json: construction, copy, destruction
// ===========================================================================

Json::Json(std::string value) {
  value_.string = new std::string(std::move(value));
  type_ = Type::String;
}

Json::Json(std::string_view value) {
  value_.string = new std::string(value);
  type_ = Type::String;
}

Json::Json(const char* value) {
  if (value == nullptr) return;
  value_.string = new std::string(value);
  type_ = Type::String;
}

Json Json::array() {
  Json out;
  out.become(Type::Array);
  return out;
}

Json Json::array(std::initializer_list<Json> elements) {
  Json out = array();
  out.value_.array->items.assign(elements.begin(), elements.end());
  return out;
}

Json Json::object() {
  Json out;
  out.become(Type::Object);
  return out;
}

Json Json::object(std::initializer_list<std::pair<std::string_view, Json>> members) {
  Json out = object();
  out.reserve(members.size());
  for (const auto& [key, value] : members) out.set(key, value);
  return out;
}

// Deep copy without recursion: a container is copied one level at a time,
// with its array/object children left null and queued until their turn.
Json::Json(const Json& other) : Json() {
  if (other.type_ == Type::String) {
    value_.string = new std::string(*other.value_.string);
    type_ = Type::String;
    return;
  }
  if (!other.is_structured()) {
    value_ = other.value_;
    type_ = other.type_;
    return;
  }

  std::vector<std::pair<Json*, const Json*>> pending;
  const auto copy_level = [&pending](Json& target, const Json& source) {
    if (source.type_ == Type::Array) {
      target.become(Type::Array);
      const std::vector<Json>& from = source.value_.array->items;
      std::vector<Json>& items = target.value_.array->items;
      items.reserve(from.size());  // no reallocation below: the queued pointers stay valid
      for (const Json& child : from) {
        if (child.is_structured()) {
          pending.emplace_back(&items.emplace_back(), &child);
        } else {
          items.push_back(child);
        }
      }
    } else {
      target.become(Type::Object);
      const ObjectNode& from = *source.value_.object;
      ObjectNode& node = *target.value_.object;
      node.members.reserve(from.members.size());
      for (const Member& member : from.members) {
        if (member.value_.is_structured()) {
          pending.emplace_back(&node.members.emplace_back(member.key_, Json()).value_, &member.value_);
        } else {
          node.members.emplace_back(member.key_, member.value_);
        }
      }
      node.slots = from.slots;  // same members at the same positions under the same secret
    }
  };

  Json root;  // owns the partial copy if an allocation fails midway
  copy_level(root, other);
  while (!pending.empty()) {
    const auto [target, source] = pending.back();
    pending.pop_back();
    copy_level(*target, *source);
  }
  swap(root);
}

// Destruction without recursion and without allocating: nested containers
// are unhooked from their parents and chained through the `doomed` link each
// node carries, so every node is deleted with only scalar/string children.
void Json::destroy() noexcept {
  if (type_ == Type::String) {
    delete value_.string;
    type_ = Type::Null;
    return;
  }
  ArrayNode* arrays = nullptr;
  ObjectNode* objects = nullptr;
  const auto unhook = [&arrays, &objects](Json& value) noexcept {
    if (value.type_ == Type::Array) {
      value.value_.array->doomed = arrays;
      arrays = value.value_.array;
      value.type_ = Type::Null;
    } else if (value.type_ == Type::Object) {
      value.value_.object->doomed = objects;
      objects = value.value_.object;
      value.type_ = Type::Null;
    }
  };
  unhook(*this);
  while (arrays != nullptr || objects != nullptr) {
    if (arrays != nullptr) {
      ArrayNode* const node = arrays;
      arrays = node->doomed;
      for (Json& child : node->items) unhook(child);
      delete node;
    } else {
      ObjectNode* const node = objects;
      objects = node->doomed;
      for (Member& member : node->members) unhook(member.value_);
      delete node;
    }
  }
}

/// Turns the value into an empty array/object unless it already is one.
void Json::become(Type type) {
  if (type_ == type) return;
  Json fresh;
  if (type == Type::Array) {
    fresh.value_.array = new ArrayNode();
  } else {
    fresh.value_.object = new ObjectNode();
  }
  fresh.type_ = type;
  swap(fresh);
}

// ===========================================================================
// Json: scalar access
// ===========================================================================

std::string_view Json::type_name() const noexcept {
  switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return "boolean";
    case Type::Int64:
    case Type::UInt64:
    case Type::Double: return "number";
    case Type::String: return "string";
    case Type::Array: return "array";
    case Type::Object: return "object";
  }
  return "null";
}

std::optional<bool> Json::get_bool() const noexcept {
  if (type_ == Type::Bool) return value_.boolean;
  return std::nullopt;
}

std::optional<std::int64_t> Json::get_int64() const noexcept {
  switch (type_) {
    case Type::Int64: return value_.int64;
    case Type::UInt64:
      if (value_.uint64 <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(value_.uint64);
      }
      return std::nullopt;
    case Type::Double: {
      // [-2^63, 2^63): both bounds are exact doubles; NaN fails the test.
      const double number = value_.float64;
      if (number >= -9223372036854775808.0 && number < 9223372036854775808.0) {
        const auto whole = static_cast<std::int64_t>(number);
        if (static_cast<double>(whole) == number) return whole;
      }
      return std::nullopt;
    }
    default: return std::nullopt;
  }
}

std::optional<std::uint64_t> Json::get_uint64() const noexcept {
  switch (type_) {
    case Type::UInt64: return value_.uint64;
    case Type::Int64:
      if (value_.int64 >= 0) return static_cast<std::uint64_t>(value_.int64);
      return std::nullopt;
    case Type::Double: {
      const double number = value_.float64;
      if (number >= 0.0 && number < 18446744073709551616.0) {
        const auto whole = static_cast<std::uint64_t>(number);
        if (static_cast<double>(whole) == number) return whole;
      }
      return std::nullopt;
    }
    default: return std::nullopt;
  }
}

std::optional<double> Json::get_double() const noexcept {
  switch (type_) {
    case Type::Double: return value_.float64;
    case Type::Int64: return static_cast<double>(value_.int64);
    case Type::UInt64: return static_cast<double>(value_.uint64);
    default: return std::nullopt;
  }
}

std::optional<std::string_view> Json::get_string() const noexcept {
  if (type_ == Type::String) return std::string_view(*value_.string);
  return std::nullopt;
}

const std::string& Json::as_string() const noexcept {
  static const std::string empty;
  return type_ == Type::String ? *value_.string : empty;
}

// ===========================================================================
// Json: containers
// ===========================================================================

std::size_t Json::size() const noexcept {
  if (type_ == Type::Array) return value_.array->items.size();
  if (type_ == Type::Object) return value_.object->members.size();
  return 0;
}

void Json::reserve(std::size_t count) {
  if (type_ == Type::Array && count <= value_.array->items.max_size()) value_.array->items.reserve(count);
  if (type_ == Type::Object && count <= value_.object->members.max_size()) value_.object->members.reserve(count);
}

std::span<const Json> Json::elements() const noexcept {
  if (type_ != Type::Array) return {};
  return value_.array->items;
}

std::span<Json> Json::elements() noexcept {
  if (type_ != Type::Array) return {};
  return value_.array->items;
}

const Json* Json::at(std::size_t index) const noexcept {
  if (type_ != Type::Array || index >= value_.array->items.size()) return nullptr;
  return &value_.array->items[index];
}

Json* Json::at(std::size_t index) noexcept {
  if (type_ != Type::Array || index >= value_.array->items.size()) return nullptr;
  return &value_.array->items[index];
}

const Json& Json::operator[](std::size_t index) const noexcept {
  const Json* element = at(index);
  return element != nullptr ? *element : shared_null();
}

Json& Json::operator[](std::size_t index) {
  become(Type::Array);
  std::vector<Json>& items = value_.array->items;
  if (index >= items.size()) {
    if (index >= items.max_size()) throw std::bad_alloc();  // cannot exist; keep the failure mode uniform
    items.resize(index + 1);
  }
  return items[index];
}

Json& Json::push_back(Json value) {
  become(Type::Array);
  return value_.array->items.emplace_back(std::move(value));
}

bool Json::erase(std::size_t index) noexcept {
  if (type_ != Type::Array || index >= value_.array->items.size()) return false;
  std::vector<Json>& items = value_.array->items;
  items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

std::span<const Json::Member> Json::items() const noexcept {
  if (type_ != Type::Object) return {};
  return value_.object->members;
}

std::span<Json::Member> Json::items() noexcept {
  if (type_ != Type::Object) return {};
  return value_.object->members;
}

const Json* Json::find(std::string_view key) const noexcept {
  if (type_ != Type::Object) return nullptr;
  const std::size_t position = value_.object->locate(key);
  return position == npos ? nullptr : &value_.object->members[position].value_;
}

Json* Json::find(std::string_view key) noexcept {
  if (type_ != Type::Object) return nullptr;
  const std::size_t position = value_.object->locate(key);
  return position == npos ? nullptr : &value_.object->members[position].value_;
}

const Json& Json::operator[](std::string_view key) const noexcept {
  const Json* value = find(key);
  return value != nullptr ? *value : shared_null();
}

Json& Json::operator[](std::string_view key) {
  if (type_ != Type::Object) {
    const std::string own(key);  // `key` may point into the value being replaced
    become(Type::Object);
    return *value_.object->find_or_append(own).first;
  }
  return *value_.object->find_or_append(key).first;
}

Json& Json::set(std::string_view key, Json value) {
  Json& slot = (*this)[key];
  slot = std::move(value);
  return slot;
}

bool Json::insert(std::string_view key, Json value) {
  if (type_ != Type::Object) {
    (*this)[key] = std::move(value);
    return true;
  }
  const auto [slot, added] = value_.object->find_or_append(key);
  if (added) *slot = std::move(value);
  return added;
}

bool Json::erase(std::string_view key) noexcept {
  if (type_ != Type::Object) return false;
  ObjectNode& node = *value_.object;
  const std::size_t position = node.locate(key);
  if (position == npos) return false;
  for (std::size_t i = position; i + 1 < node.members.size(); ++i) node.members[i] = std::move(node.members[i + 1]);
  node.members.pop_back();
  node.forget(position);
  return true;
}

// ===========================================================================
// Json: comparison
// ===========================================================================

namespace {

/// Mathematical equality across the three number kinds.
bool numbers_equal(const Json& a, const Json& b) noexcept {
  using Type = Json::Type;
  if (a.type() == b.type()) {
    if (a.type() == Type::Int64) return a.as_int64() == b.as_int64();
    if (a.type() == Type::UInt64) return a.as_uint64() == b.as_uint64();
    return a.as_double() == b.as_double();
  }
  if (a.is_float() || b.is_float()) {
    // get_*() only yields a value for a whole number in range, so comparing
    // the integers is exact where converting the integer to double is not.
    const Json& real = a.is_float() ? a : b;
    const Json& whole = a.is_float() ? b : a;
    if (whole.is_unsigned()) {
      const auto value = real.get_uint64();
      return value && *value == whole.as_uint64();
    }
    const auto value = real.get_int64();
    return value && *value == whole.as_int64();
  }
  const Json& with_sign = a.is_unsigned() ? b : a;
  const Json& without_sign = a.is_unsigned() ? a : b;
  return with_sign.as_int64() >= 0 && static_cast<std::uint64_t>(with_sign.as_int64()) == without_sign.as_uint64();
}

enum class Verdict { Equal, Different, Descend };

/// Compares everything but the children of two non-empty containers.
Verdict compare_level(const Json& a, const Json& b) noexcept {
  if (a.is_number() && b.is_number()) return numbers_equal(a, b) ? Verdict::Equal : Verdict::Different;
  if (a.type() != b.type()) return Verdict::Different;
  switch (a.type()) {
    case Json::Type::Bool: return a.as_bool() == b.as_bool() ? Verdict::Equal : Verdict::Different;
    case Json::Type::String: return a.as_string() == b.as_string() ? Verdict::Equal : Verdict::Different;
    case Json::Type::Array:
    case Json::Type::Object:
      if (a.size() != b.size()) return Verdict::Different;
      return a.empty() ? Verdict::Equal : Verdict::Descend;
    default: return Verdict::Equal;  // null
  }
}

}  // namespace

bool Json::equal(const Json& a, const Json& b, bool ordered) {
  const Verdict top = compare_level(a, b);
  if (top != Verdict::Descend) return top == Verdict::Equal;

  struct Frame {
    const Json* a;
    const Json* b;
    std::size_t next;
  };
  std::vector<Frame> stack;
  stack.push_back({&a, &b, 0});
  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.next == frame.a->size()) {
      stack.pop_back();
      continue;
    }
    const std::size_t index = frame.next++;
    const Json* left = nullptr;
    const Json* right = nullptr;
    if (frame.a->type_ == Type::Array) {
      left = &frame.a->value_.array->items[index];
      right = &frame.b->value_.array->items[index];
    } else {
      const Member& member = frame.a->value_.object->members[index];
      left = &member.value_;
      if (ordered) {
        const Member& other = frame.b->value_.object->members[index];
        if (other.key_ != member.key_) return false;
        right = &other.value_;
      } else {
        // Same member count and unique keys: finding every key of `a` in `b`
        // means the key sets are equal.
        right = frame.b->find(member.key_);
        if (right == nullptr) return false;
      }
    }
    const Verdict verdict = compare_level(*left, *right);
    if (verdict == Verdict::Different) return false;
    if (verdict == Verdict::Descend) stack.push_back({left, right, 0});
  }
  return true;
}

// ===========================================================================
// Parser
// ===========================================================================

namespace util {

namespace {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

/// Bytes that can be copied from a string literal without a closer look:
/// printable ASCII other than the quotes and the backslash.
constexpr std::array<bool, 256> parser_plain_bytes = [] {
  std::array<bool, 256> table{};
  for (std::size_t c = 0x20; c < 0x80; ++c) table[c] = c != '"' && c != '\'' && c != '\\';
  return table;
}();

/// 10^0 .. 10^22, the powers of ten a double represents exactly.
constexpr std::array<double, 23> exact_powers_of_ten = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                                        1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                                        1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

/// True when `double` arithmetic is evaluated in double precision, so one
/// multiplication or division of exact operands is correctly rounded.
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
constexpr bool double_arithmetic_is_exact = true;
#else
constexpr bool double_arithmetic_is_exact = false;
#endif

constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";

JsonLocation locate(std::string_view text, std::size_t offset) noexcept {
  JsonLocation where;
  where.offset = offset;
  std::size_t line_start = text.starts_with(utf8_bom) ? utf8_bom.size() : 0;
  for (std::size_t i = line_start; i < offset; ++i) {
    const bool lone_cr = text[i] == '\r' && !(i + 1 < text.size() && text[i + 1] == '\n');
    if (text[i] == '\n' || lone_cr) {
      ++where.line;
      line_start = i + 1;
    }
  }
  for (std::size_t i = line_start; i < offset; i += decode_utf8(text, i).length) ++where.column;
  return where;
}

class Parser {
 public:
  Parser(std::string_view text, const JsonParseOptions& options) noexcept
      : text_(text), options_(options), p_(text.data()), end_(text.data() + text.size()) {
    if (text.starts_with(utf8_bom)) p_ += utf8_bom.size();
  }

  [[nodiscard]] Result<Json> run(JsonLocation* error_location) {
    Json root;
    bool ok = parse_document(root);
    if (ok) ok = skip_space();
    if (ok && p_ != end_) ok = fail_at(p_, "trailing characters");
    if (ok) return Result<Json>(std::move(root));

    const JsonLocation where = locate(text_, static_cast<std::size_t>(where_ - text_.data()));
    if (error_location != nullptr) *error_location = where;
    return fail(Status::Decode, "invalid JSON at line {}, column {} (byte {}): {}", where.line, where.column,
                where.offset, reason_);
  }

 private:
  /// An array or object under construction, plus (for objects) the key whose
  /// value is being parsed.
  struct Frame {
    Json container;
    std::string key;
    const char* key_start = nullptr;
  };

  bool fail_at(const char* where, std::string_view reason) {
    where_ = where;
    reason_ = reason;
    return false;
  }

  [[nodiscard]] std::size_t left() const noexcept { return static_cast<std::size_t>(end_ - p_); }

  /// The first malformed UTF-8 sequence in [begin, end), or null.
  [[nodiscard]] static const char* find_invalid_utf8(const char* begin, const char* end) noexcept {
    const std::string_view range(begin, static_cast<std::size_t>(end - begin));
    for (std::size_t i = 0; i < range.size();) {
      if (static_cast<unsigned char>(range[i]) < 0x80) {
        ++i;
        continue;
      }
      const Utf8Decoded decoded = decode_utf8(range, i);
      if (!decoded.ok) return begin + i;
      i += decoded.length;
    }
    return nullptr;
  }

  /// Skips whitespace and, when enabled, comments. False on a comment error.
  bool skip_space() {
    for (;;) {
      while (p_ != end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
      if (!options_.allow_comments || left() < 2 || p_[0] != '/') return true;
      const char* body = p_ + 2;
      const char* stop = nullptr;  // end of the comment text
      const char* next = nullptr;  // where parsing resumes
      if (p_[1] == '/') {
        stop = body;
        while (stop != end_ && *stop != '\n' && *stop != '\r') ++stop;
        next = stop;
      } else if (p_[1] == '*') {
        const std::string_view rest(body, static_cast<std::size_t>(end_ - body));
        const std::size_t close = rest.find("*/");
        if (close == std::string_view::npos) return fail_at(p_, "unterminated comment");
        stop = body + close;
        next = stop + 2;
      } else {
        return true;  // a lone '/': the caller reports it
      }
      if (!options_.replace_invalid_utf8) {
        if (const char* bad = find_invalid_utf8(body, stop)) return fail_at(bad, "invalid UTF-8");
      }
      p_ = next;
    }
  }

  /// Four hex digits at `at` (which must have four bytes available).
  [[nodiscard]] static bool hex4(const char* at, char32_t& out) noexcept {
    char32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = at[i];
      char32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<char32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<char32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<char32_t>(c - 'A' + 10);
      } else {
        return false;
      }
      value = (value << 4) | digit;
    }
    out = value;
    return true;
  }

  /// Decodes the escape sequence at `p_` (a backslash with at least one
  /// byte after it) into `out`.
  bool parse_escape(std::string& out, char quote) {
    const char* const start = p_;
    const char kind = p_[1];
    p_ += 2;
    switch (kind) {
      case '"': out.push_back('"'); return true;
      case '\\': out.push_back('\\'); return true;
      case '/': out.push_back('/'); return true;
      case 'b': out.push_back('\b'); return true;
      case 'f': out.push_back('\f'); return true;
      case 'n': out.push_back('\n'); return true;
      case 'r': out.push_back('\r'); return true;
      case 't': out.push_back('\t'); return true;
      case '\'':
        if (quote != '\'') return fail_at(start, "invalid escape");
        out.push_back('\'');
        return true;
      case 'u': break;
      default: return fail_at(start, "invalid escape");
    }

    char32_t unit = 0;
    if (left() < 4 || !hex4(p_, unit)) return fail_at(start, "invalid \\u escape");
    p_ += 4;
    char32_t code_point = unit;
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      // A high surrogate must be followed by an escaped low surrogate.
      char32_t low = 0;
      if (left() >= 6 && p_[0] == '\\' && p_[1] == 'u' && hex4(p_ + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
        p_ += 6;
        code_point = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
      } else if (options_.replace_invalid_utf8) {
        code_point = replacement_character;
      } else {
        return fail_at(start, "lone surrogate");
      }
    } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
      if (!options_.replace_invalid_utf8) return fail_at(start, "lone surrogate");
      code_point = replacement_character;
    }
    append_utf8(out, code_point);
    return true;
  }

  /// Parses the rest of a string literal into `out`; `p_` is just past the
  /// opening `quote`.
  bool parse_string(std::string& out, char quote) {
    const char* const open = p_ - 1;
    const char* run = p_;  // start of the pending stretch of literal bytes
    for (;;) {
      while (p_ != end_ && parser_plain_bytes[static_cast<unsigned char>(*p_)]) ++p_;
      if (p_ == end_) return fail_at(open, "unterminated string");
      const auto byte = static_cast<unsigned char>(*p_);
      if (byte == static_cast<unsigned char>(quote)) {
        out.append(run, static_cast<std::size_t>(p_ - run));
        ++p_;
        return true;
      }
      if (byte == '\\') {
        out.append(run, static_cast<std::size_t>(p_ - run));
        if (left() < 2) return fail_at(open, "unterminated string");
        if (!parse_escape(out, quote)) return false;
        run = p_;
      } else if (byte >= 0x80) {
        const std::string_view rest(p_, left());
        const Utf8Decoded decoded = decode_utf8(rest, 0);
        if (decoded.ok) {
          p_ += decoded.length;
        } else if (options_.replace_invalid_utf8) {
          out.append(run, static_cast<std::size_t>(p_ - run));
          append_utf8(out, replacement_character);
          p_ += decoded.length;
          run = p_;
        } else {
          return fail_at(p_, "invalid UTF-8");
        }
      } else if (byte < 0x20) {
        if (!options_.allow_control_characters) return fail_at(p_, "control character in string");
        ++p_;
      } else {
        ++p_;  // the other kind of quote
      }
    }
  }

  /// An object key: a string, or with `allow_unquoted_keys` an identifier.
  bool parse_key(std::string& key) {
    key.clear();
    if (p_ == end_) return fail_at(p_, "unexpected end of input");
    if (*p_ == '"' || (*p_ == '\'' && options_.allow_single_quotes)) {
      const char quote = *p_++;
      return parse_string(key, quote);
    }
    if (options_.allow_unquoted_keys) {
      const char* const start = p_;
      const char* run = p_;
      while (p_ != end_) {
        const char c = *p_;
        if (is_ascii_alpha(c) || c == '_' || c == '$' || (is_digit(c) && p_ != start)) {
          ++p_;
        } else if (static_cast<unsigned char>(c) >= 0x80) {
          const Utf8Decoded decoded = decode_utf8(std::string_view(p_, left()), 0);
          if (decoded.ok) {
            p_ += decoded.length;
          } else if (options_.replace_invalid_utf8) {
            key.append(run, static_cast<std::size_t>(p_ - run));
            append_utf8(key, replacement_character);
            p_ += decoded.length;
            run = p_;
          } else {
            return fail_at(p_, "invalid UTF-8");
          }
        } else {
          break;
        }
      }
      if (p_ != start) {
        key.append(run, static_cast<std::size_t>(p_ - run));
        return true;
      }
    }
    return fail_at(p_, "expected a string key");
  }

  bool parse_literal(std::string_view word, Json value, Json& out) {
    if (!std::string_view(p_, left()).starts_with(word)) return fail_at(p_, "invalid literal");
    p_ += word.size();
    out = std::move(value);
    return true;
  }

  bool parse_number(Json& out) {
    const char* const start = p_;
    const bool negative = *p_ == '-';
    if (negative) ++p_;
    if (p_ == end_ || !is_digit(*p_)) return fail_at(start, "invalid number");

    // `mantissa` collects the digits for as long as they fit; `exact` says
    // whether it holds all of them, and `scale` is the power of ten to apply.
    std::uint64_t mantissa = 0;
    bool exact = true;
    std::int64_t scale = 0;
    const auto take_digits = [&](bool fraction) noexcept {
      for (; p_ != end_ && is_digit(*p_); ++p_) {
        const auto digit = static_cast<std::uint64_t>(*p_ - '0');
        if (exact && mantissa <= (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
          mantissa = mantissa * 10 + digit;
          if (fraction) --scale;
        } else {
          exact = false;
        }
      }
    };

    if (*p_ == '0') {
      ++p_;
      if (p_ != end_ && is_digit(*p_)) return fail_at(start, "invalid number");  // leading zero
    } else {
      take_digits(false);
    }
    bool integer = true;
    if (p_ != end_ && *p_ == '.') {
      integer = false;
      ++p_;
      if (p_ == end_ || !is_digit(*p_)) return fail_at(start, "invalid number");
      take_digits(true);
    }
    if (p_ != end_ && (*p_ == 'e' || *p_ == 'E')) {
      integer = false;
      ++p_;
      bool negative_exponent = false;
      if (p_ != end_ && (*p_ == '+' || *p_ == '-')) negative_exponent = *p_++ == '-';
      if (p_ == end_ || !is_digit(*p_)) return fail_at(start, "invalid number");
      std::int64_t exponent = 0;
      for (; p_ != end_ && is_digit(*p_); ++p_) {
        if (exponent < 100'000'000) {
          exponent = exponent * 10 + (*p_ - '0');
        } else {
          exact = false;  // absurd exponent: leave it to the general algorithm
        }
      }
      scale += negative_exponent ? -exponent : exponent;
    }

    if (integer && exact) {
      constexpr std::uint64_t int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
      if (!negative) {
        out = mantissa <= int64_max ? Json(static_cast<std::int64_t>(mantissa)) : Json(mantissa);
        return true;
      }
      if (mantissa <= int64_max + 1) {
        out = Json(static_cast<std::int64_t>(0 - mantissa));  // two's complement; exact for -2^63 as well
        return true;
      }
    }

    // Up to 2^53 times or divided by an exact power of ten is one correctly
    // rounded operation; everything else goes through the general algorithm.
    if (double_arithmetic_is_exact && exact && mantissa <= (std::uint64_t{1} << 53) && scale >= -22 && scale <= 22) {
      double value = static_cast<double>(mantissa);
      if (scale < 0) {
        value /= exact_powers_of_ten[static_cast<std::size_t>(-scale)];
      } else {
        value *= exact_powers_of_ten[static_cast<std::size_t>(scale)];
      }
      out = Json(negative ? -value : value);
      return true;
    }
    const std::optional<double> value = parse_double(std::string_view(start, static_cast<std::size_t>(p_ - start)));
    if (!value) return fail_at(start, "invalid number");
    if (std::isinf(*value)) return fail_at(start, "number out of range");
    out = Json(*value);
    return true;
  }

  /// Parses a string, number or literal at `p_` (which is not at the end).
  bool parse_scalar(Json& out) {
    const char c = *p_;
    switch (c) {
      case '"': break;
      case '\'':
        if (!options_.allow_single_quotes) return fail_at(p_, "unexpected character");
        break;
      case 't': return parse_literal("true", Json(true), out);
      case 'f': return parse_literal("false", Json(false), out);
      case 'n': return parse_literal("null", Json(), out);
      case 'N':
        if (!options_.allow_nan_inf) return fail_at(p_, "unexpected character");
        return parse_literal("NaN", Json(std::numeric_limits<double>::quiet_NaN()), out);
      case 'I':
        if (!options_.allow_nan_inf) return fail_at(p_, "unexpected character");
        return parse_literal("Infinity", Json(std::numeric_limits<double>::infinity()), out);
      case '-':
        if (options_.allow_nan_inf && left() >= 2 && p_[1] == 'I') {
          return parse_literal("-Infinity", Json(-std::numeric_limits<double>::infinity()), out);
        }
        return parse_number(out);
      default:
        if (is_digit(c)) return parse_number(out);
        return fail_at(p_, "unexpected character");
    }
    ++p_;
    std::string text;
    if (!parse_string(text, c)) return false;
    out = Json(std::move(text));
    return true;
  }

  /// The whole grammar as a small state machine over an explicit stack of
  /// open containers, so nesting depth costs heap, never call stack.
  bool parse_document(Json& root) {
    enum class State { Value, Key, Close, Attach };
    std::vector<Frame> stack;
    Json value;
    State state = State::Value;
    for (;;) {
      switch (state) {
        case State::Value: {
          if (!skip_space()) return false;
          if (p_ == end_) return fail_at(p_, "unexpected end of input");
          const char c = *p_;
          if (c != '[' && c != '{') {
            if (!parse_scalar(value)) return false;
            state = State::Attach;
            break;
          }
          if (stack.size() >= options_.max_depth) {
            return fail_at(p_, std::format("nesting too deep (limit {})", options_.max_depth));
          }
          ++p_;
          stack.push_back(Frame{c == '[' ? Json::array() : Json::object(), {}, nullptr});
          if (!skip_space()) return false;
          if (p_ != end_ && *p_ == (c == '[' ? ']' : '}')) {
            ++p_;
            state = State::Close;
          } else {
            state = c == '[' ? State::Value : State::Key;
          }
          break;
        }
        case State::Key: {
          if (!skip_space()) return false;
          stack.back().key_start = p_;
          if (!parse_key(stack.back().key)) return false;
          if (!skip_space()) return false;
          if (p_ == end_) return fail_at(p_, "unexpected end of input");
          if (*p_ != ':') return fail_at(p_, "expected ':'");
          ++p_;
          state = State::Value;
          break;
        }
        case State::Close: {
          value = std::move(stack.back().container);
          stack.pop_back();
          state = State::Attach;
          break;
        }
        case State::Attach: {
          if (stack.empty()) {
            root = std::move(value);
            return true;
          }
          Frame& top = stack.back();
          const bool in_array = top.container.is_array();
          if (in_array) {
            top.container.push_back(std::move(value));
          } else if (options_.duplicate_keys == JsonDuplicateKeys::LastWins) {
            top.container.set(top.key, std::move(value));
          } else if (!top.container.insert(top.key, std::move(value)) &&
                     options_.duplicate_keys == JsonDuplicateKeys::Reject) {
            return fail_at(top.key_start, "duplicate key");
          }

          if (!skip_space()) return false;
          if (p_ == end_) return fail_at(p_, "unexpected end of input");
          const char close = in_array ? ']' : '}';
          if (*p_ == ',') {
            ++p_;
            if (options_.allow_trailing_commas) {
              if (!skip_space()) return false;
              if (p_ != end_ && *p_ == close) {
                ++p_;
                state = State::Close;
                break;
              }
            }
            state = in_array ? State::Value : State::Key;
          } else if (*p_ == close) {
            ++p_;
            state = State::Close;
          } else {
            return fail_at(p_, in_array ? "expected ',' or ']'" : "expected ',' or '}'");
          }
          break;
        }
      }
    }
  }

  std::string_view text_;
  const JsonParseOptions& options_;
  const char* p_;
  const char* end_;
  const char* where_ = nullptr;
  std::string reason_;
};

}  // namespace

Result<Json> parse_json(std::string_view text, std::size_t max_depth) {
  JsonParseOptions options;
  options.max_depth = max_depth;
  return Parser(text, options).run(nullptr);
}

Result<Json> parse_json(std::string_view text, const JsonParseOptions& options, JsonLocation* error_location) {
  return Parser(text, options).run(error_location);
}

// ===========================================================================
// Serialiser
// ===========================================================================

namespace {

/// Bytes a string literal can carry unescaped without a closer look:
/// printable ASCII other than '"', '\\' and '/'.
constexpr std::array<bool, 256> writer_plain_bytes = [] {
  std::array<bool, 256> table{};
  for (std::size_t c = 0x20; c < 0x7F; ++c) table[c] = c != '"' && c != '\\' && c != '/';
  return table;
}();

class Writer {
 public:
  Writer(std::string& out, const JsonWriteOptions& options) noexcept
      : out_(out),
        options_(options),
        pretty_(options.indent >= 0),
        indent_(options.indent > 0 ? static_cast<std::size_t>(options.indent) : 0) {}

  /// Depth-first over an explicit stack: one frame per open container.
  void write(const Json& root) {
    struct Frame {
      std::span<const Json> elements;         // array
      std::span<const Json::Member> members;  // object
      std::size_t count = 0;
      std::size_t next = 0;
      std::vector<std::size_t> order;  // member order when sorting keys
    };
    std::vector<Frame> stack;
    const auto open = [&](const Json& container) {
      Frame& frame = stack.emplace_back();
      frame.elements = container.elements();
      frame.members = container.items();
      frame.count = container.size();
      if (options_.sort_keys && container.is_object()) {
        frame.order.resize(frame.count);
        for (std::size_t i = 0; i < frame.count; ++i) frame.order[i] = i;
        std::ranges::sort(frame.order, {},
                          [&](std::size_t i) -> std::string_view { return frame.members[i].key(); });
      }
    };

    if (begin_value(root)) open(root);
    while (!stack.empty()) {
      Frame& frame = stack.back();
      const bool is_array = frame.members.empty();
      if (frame.next == frame.count) {
        stack.pop_back();
        line_break(stack.size());
        out_.push_back(is_array ? ']' : '}');
        continue;
      }
      if (frame.next != 0) out_.push_back(',');
      line_break(stack.size());
      const Json* child = nullptr;
      if (is_array) {
        child = &frame.elements[frame.next];
      } else {
        const Json::Member& member = frame.members[frame.order.empty() ? frame.next : frame.order[frame.next]];
        append_string(member.key());
        out_.push_back(':');
        if (pretty_) out_.push_back(' ');
        child = &member.value();
      }
      ++frame.next;
      if (begin_value(*child)) open(*child);  // invalidates `frame`
    }
  }

 private:
  /// Writes a scalar or an empty container in full and returns false; for a
  /// non-empty container writes the opening bracket and returns true.
  bool begin_value(const Json& value) {
    switch (value.type()) {
      case Json::Type::Null: out_.append("null"); return false;
      case Json::Type::Bool: out_.append(value.as_bool() ? "true" : "false"); return false;
      case Json::Type::Int64: append_integer(value.as_int64()); return false;
      case Json::Type::UInt64: append_integer(value.as_uint64()); return false;
      case Json::Type::Double: append_double(value.as_double()); return false;
      case Json::Type::String: append_string(value.as_string()); return false;
      case Json::Type::Array:
        if (value.empty()) {
          out_.append("[]");
          return false;
        }
        out_.push_back('[');
        return true;
      case Json::Type::Object:
        if (value.empty()) {
          out_.append("{}");
          return false;
        }
        out_.push_back('{');
        return true;
    }
    return false;
  }

  void line_break(std::size_t depth) {
    if (!pretty_) return;
    out_.append(options_.newline);
    out_.append(depth * indent_, ' ');
  }

  template <class Integer>
  void append_integer(Integer value) {
    std::array<char, 24> buffer{};
    const char* const end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value).ptr;
    out_.append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
  }

  void append_double(double value) {
    if (!std::isfinite(value)) {
      if (!options_.nan_inf_as_literals) {
        out_.append("null");
      } else if (std::isnan(value)) {
        out_.append("NaN");
      } else {
        out_.append(value < 0 ? "-Infinity" : "Infinity");
      }
      return;
    }
    // Shortest round-trip digits in the form [-]d[.ddd]e[+-]XX, re-laid out
    // positionally for everyday magnitudes.
    std::array<char, 40> buffer{};
    const char* const end =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific).ptr;
    const char* p = buffer.data();
    if (*p == '-') out_.push_back(*p++);
    const char* const mantissa = p;
    std::array<char, 20> digits{};
    std::size_t count = 0;
    for (; p != end && *p != 'e'; ++p) {
      if (*p != '.' && count < digits.size()) digits[count++] = *p;
    }
    int exponent = 0;
    if (p != end) {
      const char* e = p + 1;
      const bool negative = e != end && *e == '-';
      if (e != end && (*e == '-' || *e == '+')) ++e;
      for (; e != end; ++e) exponent = exponent * 10 + (*e - '0');
      if (negative) exponent = -exponent;
    }
    if (exponent < -4 || exponent >= 16) {
      out_.append(mantissa, static_cast<std::size_t>(end - mantissa));
    } else if (exponent < 0) {
      out_.append("0.");
      out_.append(static_cast<std::size_t>(-exponent - 1), '0');
      out_.append(digits.data(), count);
    } else {
      const std::size_t whole = static_cast<std::size_t>(exponent) + 1;
      if (count <= whole) {
        out_.append(digits.data(), count);
        out_.append(whole - count, '0');
        out_.append(".0");
      } else {
        out_.append(digits.data(), whole);
        out_.push_back('.');
        out_.append(digits.data() + whole, count - whole);
      }
    }
  }

  void append_unicode_escape(char32_t unit) {
    constexpr std::string_view hex = "0123456789abcdef";
    const std::array<char, 6> text = {'\\', 'u', hex[(unit >> 12) & 0xF], hex[(unit >> 8) & 0xF], hex[(unit >> 4) & 0xF],
                                      hex[unit & 0xF]};
    out_.append(text.data(), text.size());
  }

  void append_string(std::string_view text) {
    out_.push_back('"');
    std::size_t run = 0;  // start of the pending stretch of bytes copied verbatim
    std::size_t i = 0;
    const auto flush = [&] { out_.append(text.data() + run, i - run); };
    while (i < text.size()) {
      const auto byte = static_cast<unsigned char>(text[i]);
      if (writer_plain_bytes[byte]) {
        ++i;
        continue;
      }
      if (byte >= 0x80) {
        const Utf8Decoded decoded = decode_utf8(text, i);
        if (decoded.ok && !options_.ascii_only) {
          i += decoded.length;
          continue;
        }
        flush();
        if (!options_.ascii_only) {
          append_utf8(out_, replacement_character);
        } else if (decoded.code_point < 0x10000) {
          append_unicode_escape(decoded.code_point);
        } else {
          const char32_t offset = decoded.code_point - 0x10000;
          append_unicode_escape(0xD800 + (offset >> 10));
          append_unicode_escape(0xDC00 + (offset & 0x3FF));
        }
        i += decoded.length;
        run = i;
        continue;
      }
      if ((byte == '/' && !options_.escape_slash) || (byte == 0x7F && !options_.ascii_only)) {
        ++i;
        continue;
      }
      flush();
      switch (byte) {
        case '"': out_.append("\\\""); break;
        case '\\': out_.append("\\\\"); break;
        case '/': out_.append("\\/"); break;
        case '\b': out_.append("\\b"); break;
        case '\f': out_.append("\\f"); break;
        case '\n': out_.append("\\n"); break;
        case '\r': out_.append("\\r"); break;
        case '\t': out_.append("\\t"); break;
        default: append_unicode_escape(byte); break;  // other controls, and DEL in ASCII mode
      }
      ++i;
      run = i;
    }
    flush();
    out_.push_back('"');
  }

  std::string& out_;
  const JsonWriteOptions& options_;
  bool pretty_;
  std::size_t indent_;
};

}  // namespace

std::string dump_json(const Json& value, int indent) {
  JsonWriteOptions options;
  options.indent = indent;
  return dump_json(value, options);
}

std::string dump_json(const Json& value, const JsonWriteOptions& options) {
  std::string out;
  Writer(out, options).write(value);
  return out;
}

}  // namespace util
}  // namespace cvt
