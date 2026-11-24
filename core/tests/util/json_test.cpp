// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "support/test_support.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace cvt::util;
using namespace std::string_view_literals;

// The API promises checked at compile time.
static_assert(sizeof(Json) <= 16);
static_assert(std::is_nothrow_default_constructible_v<Json>);
static_assert(std::is_nothrow_move_constructible_v<Json>);
static_assert(std::is_nothrow_move_assignable_v<Json>);
static_assert(std::is_nothrow_swappable_v<Json>);
static_assert(std::is_nothrow_move_constructible_v<Json::Member>);
static_assert(std::is_copy_constructible_v<Json> && std::is_copy_assignable_v<Json>);
// Conversions that would be surprising do not exist.
static_assert(!std::is_constructible_v<Json, char>);
static_assert(!std::is_constructible_v<Json, char32_t>);
static_assert(!std::is_constructible_v<Json, void*>);
static_assert(!std::is_constructible_v<Json, const int*>);
static_assert(std::is_constructible_v<Json, signed char>);
static_assert(std::is_constructible_v<Json, std::vector<std::vector<std::string>>>);
static_assert(!std::is_constructible_v<Json, std::vector<void*>>);
// A path is a range of paths; asking the question must compile (the answer
// differs by platform, since a POSIX path converts to std::string).
[[maybe_unused]] constexpr bool path_question_compiles = std::is_constructible_v<Json, std::filesystem::path>;
// Members cannot be reassigned from outside: keys are fixed once inserted.
static_assert(!std::is_assignable_v<Json::Member&, const Json::Member&>);
static_assert(!std::is_assignable_v<Json::Member&, Json::Member&&>);

namespace {

constexpr double nan_value = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr std::int64_t int64_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t int64_max = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t uint64_max = std::numeric_limits<std::uint64_t>::max();

/// The compact form of the parsed text, or "error: <reason>".
std::string reparse(std::string_view text, const JsonParseOptions& options = {}) {
  const auto parsed = parse_json(text, options);
  if (parsed) return dump_json(*parsed);
  const std::string& message = parsed.error().message;
  const std::size_t reason = message.find("): ");
  return "error: " + (reason == std::string::npos ? message : message.substr(reason + 3));
}

bool accepts(std::string_view text, const JsonParseOptions& options = {}) {
  return parse_json(text, options).has_value();
}

/// A parsed literal for building expectations; a marker string on failure.
Json json(std::string_view text) {
  auto parsed = parse_json(text);
  return parsed ? std::move(*parsed) : Json("<invalid test literal>");
}

std::vector<std::string> keys_of(const Json& object) {
  std::vector<std::string> keys;
  for (const auto& [key, value] : object.items()) keys.push_back(key);
  return keys;
}

std::string repeat(std::string_view unit, std::size_t count) {
  std::string out;
  out.reserve(unit.size() * count);
  for (std::size_t i = 0; i < count; ++i) out.append(unit);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The original contract of parse_json / dump_json
// ---------------------------------------------------------------------------

TEST_CASE("json: parsing keeps key order and reports errors as values") {
  const auto parsed = parse_json(R"({"z":1,"a":[true,null,"x"],"m":{"k":1.5}})");
  REQUIRE(parsed);
  CHECK(parsed->is_object());
  std::vector<std::string> keys;
  for (const auto& item : parsed->items()) keys.push_back(item.key());
  CHECK(keys == std::vector<std::string>{"z", "a", "m"});
  CHECK(dump_json(*parsed) == R"({"z":1,"a":[true,null,"x"],"m":{"k":1.5}})");
  CHECK(dump_json(*parsed, 2).find("\n  \"z\": 1") != std::string::npos);

  CHECK(parse_json("\xEF\xBB\xBF[1]")->size() == 1);  // BOM tolerated
  CHECK((*parse_json("  42 ") == Json(42)));
  for (const std::string_view bad : {"", "{", "[1,]", "{\"a\":}", "nul", "\"unterminated", "{\"a\":1} trailing", "\"\xFF\""}) {
    const auto result = parse_json(bad);
    CHECK_MESSAGE(!result, "should reject: ", bad);
    if (!result) CHECK(result.error().code == Status::Decode);
  }
}

TEST_CASE("json: nesting depth is bounded") {
  const std::string deep = std::string(600, '[') + std::string(600, ']');
  CHECK(parse_json(deep).error().code == Status::Decode);
  CHECK(parse_json(deep, 1000).has_value());
  const std::string shallow = std::string(100, '[') + std::string(100, ']');
  CHECK(parse_json(shallow).has_value());
  // Brackets inside strings do not count.
  CHECK(parse_json("\"" + std::string(1000, '[') + "\"").has_value());
  CHECK(parse_json("\"\\\"" + std::string(1000, '{') + "\"").has_value());
  // A hostile, unterminated tower of brackets fails cleanly.
  CHECK_FALSE(parse_json(std::string(200000, '[')));

  // The limit is the number of containers open at once, arrays and objects alike.
  CHECK(default_json_depth == 512);
  CHECK(parse_json(std::string(512, '[') + std::string(512, ']')).has_value());
  CHECK_FALSE(parse_json(std::string(513, '[') + std::string(513, ']')));
  CHECK(parse_json(repeat("{\"a\":", 512) + "1" + std::string(512, '}')).has_value());
  CHECK_FALSE(parse_json(repeat("{\"a\":", 513) + "1" + std::string(513, '}')));
  CHECK(reparse(repeat("{\"a\":[", 257) + "1").starts_with("error: nesting too deep"));
  CHECK(parse_json("[[1],[2],[[3]]]", 3).has_value());  // siblings do not add up
  CHECK_FALSE(parse_json("[[1],[2],[[3]]]", 2));
  CHECK(parse_json("[]", 1).has_value());
  CHECK_FALSE(parse_json("[]", 0));
  CHECK_FALSE(parse_json("{}", 0));
  CHECK(parse_json("\"scalar\"", 0).has_value());
  CHECK(parse_json(" 17 ", 0).has_value());
  CHECK(reparse("[[[]]]", JsonParseOptions{.max_depth = 2}) == "error: nesting too deep (limit 2)");
}

TEST_CASE("json: dumping never throws on invalid UTF-8") {
  Json value = Json::object();
  value["bad"] = std::string("a\xFFz");
  CHECK(dump_json(value) == "{\"bad\":\"a\xEF\xBF\xBDz\"}");
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

TEST_CASE("json: construction and type queries") {
  const Json null_value;
  CHECK(null_value.type() == Json::Type::Null);
  CHECK(null_value.is_null());
  CHECK(Json(nullptr).is_null());
  CHECK(Json(static_cast<const char*>(nullptr)).is_null());
  CHECK(std::string(null_value.type_name()) == "null");
  CHECK_FALSE(null_value.is_structured());

  const Json truth = true;
  CHECK(truth.type() == Json::Type::Bool);
  CHECK(truth.is_bool());
  CHECK_FALSE(truth.is_number());
  CHECK(std::string(truth.type_name()) == "boolean");

  const Json signed_value = -5;
  CHECK(signed_value.type() == Json::Type::Int64);
  CHECK(signed_value.is_number());
  CHECK(signed_value.is_integer());
  CHECK_FALSE(signed_value.is_unsigned());
  CHECK_FALSE(signed_value.is_float());
  CHECK(Json(std::int8_t{-5}).type() == Json::Type::Int64);
  CHECK(Json(short{7}).type() == Json::Type::Int64);
  CHECK(Json(int64_min).as_int64() == int64_min);

  const Json unsigned_value = 5u;
  CHECK(unsigned_value.type() == Json::Type::UInt64);
  CHECK(unsigned_value.is_integer());
  CHECK(unsigned_value.is_unsigned());
  CHECK(Json(std::uint8_t{200}).type() == Json::Type::UInt64);
  CHECK(Json(std::size_t{7}).type() == Json::Type::UInt64);
  CHECK(Json(uint64_max).as_uint64() == uint64_max);

  const Json real = 1.5;
  CHECK(real.type() == Json::Type::Double);
  CHECK(real.is_number());
  CHECK(real.is_float());
  CHECK_FALSE(real.is_integer());
  CHECK(Json(2.0f).type() == Json::Type::Double);
  CHECK(Json(1.0).is_float());  // a whole double is still a double
  CHECK(std::string(real.type_name()) == "number");
  CHECK(std::string(signed_value.type_name()) == "number");

  CHECK(Json("literal").is_string());
  CHECK(Json(std::string("owned")).as_string() == "owned");
  CHECK(Json("view"sv).as_string() == "view");
  CHECK(Json("nul\0inside"sv).as_string().size() == 10);
  CHECK(std::string(Json("x").type_name()) == "string");

  const Json list = Json::array();
  CHECK(list.type() == Json::Type::Array);
  CHECK(list.is_array());
  CHECK(list.is_structured());
  CHECK(list.empty());
  CHECK(std::string(list.type_name()) == "array");
  const Json map = Json::object();
  CHECK(map.type() == Json::Type::Object);
  CHECK(map.is_object());
  CHECK(map.is_structured());
  CHECK(map.empty());
  CHECK(std::string(map.type_name()) == "object");
}

TEST_CASE("json: builders and ranges") {
  CHECK(dump_json(Json::array({1, 2.5, "three", nullptr, true, Json::array(), Json::object()})) ==
        R"([1,2.5,"three",null,true,[],{}])");
  CHECK(dump_json(Json::array({})) == "[]");
  const Json built = Json::object({{"k", 1}, {"n", nullptr}, {"list", Json::array({1, 2, 3})}, {"nested", Json::object({{"deep", true}})}});
  CHECK(dump_json(built) == R"({"k":1,"n":null,"list":[1,2,3],"nested":{"deep":true}})");
  CHECK(dump_json(Json::object({})) == "{}");
  // A repeated key keeps its first position and takes the last value.
  CHECK(dump_json(Json::object({{"a", 1}, {"b", 2}, {"a", 3}})) == R"({"a":3,"b":2})");

  CHECK(dump_json(Json(std::vector<int>{1, 2, 3})) == "[1,2,3]");
  CHECK(dump_json(Json(std::vector<std::string>{"a", "b"})) == R"(["a","b"])");
  CHECK(dump_json(Json(std::vector<std::string>{})) == "[]");
  CHECK(dump_json(Json(std::vector<std::vector<double>>{{1.0, 2.5}, {}, {3.0}})) == "[[1.0,2.5],[],[3.0]]");
  CHECK(dump_json(Json(std::vector<bool>{true, false})) == "[true,false]");
  CHECK(dump_json(Json(std::array<std::uint8_t, 3>{1, 2, 255})) == "[1,2,255]");
  const std::array<std::string_view, 2> views = {"x", "y"};
  CHECK(dump_json(Json(std::span<const std::string_view>(views))) == R"(["x","y"])");
  CHECK(dump_json(Json(std::vector<Json>{Json(1), Json("two")})) == R"([1,"two"])");
  // Strings are strings, not ranges of characters.
  CHECK(Json(std::string("abc")).is_string());
  const char buffer[] = "abc";
  CHECK(Json(buffer).is_string());

  // Implicit conversions make assignments read naturally.
  Json entry = Json::object();
  entry["id"] = std::string("png");
  entry["count"] = 3;
  entry["ratio"] = 0.5;
  entry["on"] = true;
  entry["none"] = nullptr;
  entry["tags"] = std::vector<std::string>{"a", "b"};
  CHECK(dump_json(entry) == R"({"id":"png","count":3,"ratio":0.5,"on":true,"none":null,"tags":["a","b"]})");
}

TEST_CASE("json: scalar getters convert only when exact") {
  // Booleans and strings are never coerced.
  CHECK(Json(true).as_bool());
  CHECK_FALSE(Json(false).as_bool(true));
  CHECK(Json(1).as_bool(true));
  CHECK_FALSE(Json(1).as_bool());
  CHECK_FALSE(Json("true").as_bool());
  CHECK_FALSE(Json(1).get_bool().has_value());
  CHECK(Json(true).get_bool().value_or(false));
  CHECK(Json(true).as_int64(9) == 9);
  CHECK(Json("1").as_int64(9) == 9);
  CHECK(Json().as_int64(9) == 9);
  CHECK(Json("1").as_double(2.5) == 2.5);
  CHECK(Json("text").as_string() == "text");
  CHECK(Json(1).as_string().empty());
  CHECK(Json().as_string().empty());
  CHECK(std::string(Json("text").get_string().value_or("?")) == "text");
  CHECK_FALSE(Json(5).get_string().has_value());
  CHECK_FALSE(Json().get_string().has_value());

  // Integers.
  CHECK(Json(5).get_int64().value_or(-1) == 5);
  CHECK(Json(5).get_uint64().value_or(99) == 5);
  CHECK(Json(5).get_double().value_or(-1.0) == 5.0);
  CHECK(Json(5u).as_int64() == 5);
  CHECK(Json(-5).as_int64() == -5);
  CHECK_FALSE(Json(-5).get_uint64().has_value());
  CHECK(Json(-5).as_uint64(7) == 7);
  CHECK_FALSE(Json(uint64_max).get_int64().has_value());
  CHECK(Json(uint64_max).as_int64(-1) == -1);
  CHECK(Json(static_cast<std::uint64_t>(int64_max)).as_int64() == int64_max);
  CHECK_FALSE(Json(static_cast<std::uint64_t>(int64_max) + 1).get_int64().has_value());
  CHECK(Json(uint64_max).as_double() == 18446744073709551616.0);  // nearest double
  CHECK(Json(int64_min).as_double() == -9223372036854775808.0);

  // Doubles convert to integers only when they are whole and in range.
  CHECK(Json(3.0).get_int64().value_or(-1) == 3);
  CHECK(Json(3.0).get_uint64().value_or(99) == 3);
  CHECK_FALSE(Json(3.5).get_int64().has_value());
  CHECK(Json(3.5).as_int64(-1) == -1);
  CHECK(Json(-0.0).get_int64().value_or(-1) == 0);
  CHECK(Json(-0.0).get_uint64().value_or(99) == 0);
  CHECK(Json(-3.0).as_int64() == -3);
  CHECK_FALSE(Json(-3.0).get_uint64().has_value());
  CHECK_FALSE(Json(-1e-9).get_uint64().has_value());
  CHECK(Json(-9223372036854775808.0).get_int64().value_or(0) == int64_min);
  CHECK_FALSE(Json(9223372036854775808.0).get_int64().has_value());
  CHECK(Json(9223372036854775808.0).get_uint64().value_or(0) == 9223372036854775808ull);
  CHECK(Json(18446744073709549568.0).get_uint64().value_or(0) == 18446744073709549568ull);  // largest double below 2^64
  CHECK_FALSE(Json(18446744073709551616.0).get_uint64().has_value());
  CHECK_FALSE(Json(1e300).get_int64().has_value());
  CHECK_FALSE(Json(-1e300).get_int64().has_value());
  CHECK_FALSE(Json(nan_value).get_int64().has_value());
  CHECK_FALSE(Json(nan_value).get_uint64().has_value());
  CHECK_FALSE(Json(infinity).get_int64().has_value());
  CHECK_FALSE(Json(-infinity).get_uint64().has_value());
  CHECK(Json(infinity).as_double() == infinity);
  CHECK(std::isnan(Json(nan_value).as_double(1.0)));
  CHECK(std::signbit(Json(-0.0).as_double()));
}

TEST_CASE("json: arrays") {
  Json list = Json::array();
  CHECK(list.push_back(1).as_int64() == 1);
  list.push_back("two");
  list.push_back(Json::array({3}));
  CHECK(list.size() == 3);
  CHECK_FALSE(list.empty());
  CHECK(dump_json(list) == R"([1,"two",[3]])");

  const Json& view = list;
  CHECK(view[0].as_int64() == 1);
  CHECK(view[1].as_string() == "two");
  CHECK(view[2][0].as_int64() == 3);
  CHECK(view[3].is_null());  // out of range: a shared null, no growth
  CHECK(view[1000000].is_null());
  CHECK(view.size() == 3);
  CHECK(view.at(2) != nullptr);
  CHECK(view.at(3) == nullptr);
  CHECK(list.at(0) == &list[0]);

  // Non-const indexing extends with nulls.
  list[5] = true;
  CHECK(dump_json(list) == R"([1,"two",[3],null,null,true])");
  list[0] = "first";
  CHECK(list[0].as_string() == "first");

  std::size_t count = 0;
  for (const Json& element : view.elements()) count += element.is_null() ? 0 : 1;
  CHECK(count == 4);
  for (Json& element : list.elements()) {
    if (element.is_null()) element = 0;
  }
  CHECK(dump_json(list) == R"(["first","two",[3],0,0,true])");

  CHECK(list.erase(std::size_t{1}));
  CHECK(list.erase(0));
  CHECK_FALSE(list.erase(99));
  CHECK(dump_json(list) == "[[3],0,0,true]");
  while (list.erase(std::size_t{0})) {
  }
  CHECK(list.empty());
  CHECK(list.is_array());

  Json reserved = Json::array();
  reserved.reserve(1000);
  for (int i = 0; i < 1000; ++i) reserved.push_back(i);
  CHECK(reserved.size() == 1000);
  CHECK(reserved[999].as_int64() == 999);
}

TEST_CASE("json: objects keep insertion order and unique keys") {
  Json object = Json::object();
  object["zebra"] = 1;
  object["apple"] = 2;
  object.set("mango", 3);
  CHECK(object.size() == 3);
  CHECK(keys_of(object) == std::vector<std::string>{"zebra", "apple", "mango"});

  // Replacing keeps the position.
  object["zebra"] = "striped";
  object.set("apple", Json::array({1}));
  CHECK(dump_json(object) == R"({"zebra":"striped","apple":[1],"mango":3})");
  CHECK(object.set("mango", 4).as_int64() == 4);
  CHECK(object.size() == 3);

  // insert() only adds.
  CHECK_FALSE(object.insert("mango", "ignored"));
  CHECK(object["mango"].as_int64() == 4);
  CHECK(object.insert("kiwi", 5));
  CHECK(keys_of(object) == std::vector<std::string>{"zebra", "apple", "mango", "kiwi"});

  // Lookup.
  const Json& view = object;
  CHECK(view.contains("kiwi"));
  CHECK_FALSE(view.contains("Kiwi"));
  CHECK_FALSE(view.contains(""));
  CHECK(view.find("apple") != nullptr);
  CHECK(view.find("pear") == nullptr);
  CHECK(view["pear"].is_null());  // const access never inserts
  CHECK(view.size() == 4);
  CHECK(object.find("kiwi") == &object["kiwi"]);
  *object.find("kiwi") = 6;
  CHECK(view["kiwi"].as_int64() == 6);

  // Non-const access inserts null, like a map.
  CHECK(object["new"].is_null());
  CHECK(view.contains("new"));
  CHECK(object.size() == 5);

  // Erasing keeps the order of the rest.
  CHECK(object.erase("apple"));
  CHECK_FALSE(object.erase("apple"));
  CHECK_FALSE(object.erase("never there"));
  CHECK(keys_of(object) == std::vector<std::string>{"zebra", "mango", "kiwi", "new"});

  // Keys are arbitrary strings: empty, with NUL, non-ASCII, and view-typed.
  object[""] = "empty";
  object["a\0b"sv] = "nul";
  object["caf\xC3\xA9"] = "utf-8";
  const std::string owned = "owned key";
  object[owned] = 1;
  CHECK(view[""].as_string() == "empty");
  CHECK(view["a\0b"sv].as_string() == "nul");
  CHECK_FALSE(view.contains("a"));
  CHECK(view["caf\xC3\xA9"].as_string() == "utf-8");
  CHECK(view.contains(owned));

  // Iteration: structured bindings, accessors, and in-place modification.
  Json counts = Json::object({{"a", 1}, {"b", 2}, {"c", 3}});
  std::string seen;
  std::int64_t sum = 0;
  for (const auto& [key, value] : std::as_const(counts).items()) {
    seen += key;
    sum += value.as_int64();
  }
  CHECK(seen == "abc");
  CHECK(sum == 6);
  for (auto& [key, value] : counts.items()) value = value.as_int64() * 10;
  for (Json::Member& member : counts.items()) member.value() = member.value().as_int64() + 1;
  CHECK(dump_json(counts) == R"({"a":11,"b":21,"c":31})");
  CHECK(counts.items()[1].key() == "b");
  CHECK(std::as_const(counts).items()[2].value().as_int64() == 31);
}

TEST_CASE("json: large objects stay ordered and are found through the index") {
  // Enough members to cross the small-object threshold several times over.
  Json object = Json::object();
  for (int i = 0; i < 2000; ++i) object.set("key" + std::to_string(i), i);
  CHECK(object.size() == 2000);
  bool all_found = true;
  for (int i = 0; i < 2000; ++i) all_found = all_found && object["key" + std::to_string(i)].as_int64(-1) == i;
  CHECK(all_found);
  CHECK_FALSE(object.contains("key2000"));
  CHECK_FALSE(object.contains("key"));
  CHECK_FALSE(object.contains(""));
  std::size_t position = 0;
  bool ordered = true;
  for (const auto& [key, value] : object.items()) ordered = ordered && key == "key" + std::to_string(position++);
  CHECK(ordered);

  // Replacing does not move or duplicate.
  for (int i = 0; i < 2000; i += 7) object["key" + std::to_string(i)] = "replaced";
  CHECK(object.size() == 2000);
  CHECK(object.items()[7].key() == "key7");
  CHECK(object.items()[7].value().as_string() == "replaced");

  // Erase every other member; the rest keep their order and stay reachable.
  for (int i = 0; i < 2000; i += 2) CHECK(object.erase("key" + std::to_string(i)));
  CHECK(object.size() == 1000);
  bool consistent = true;
  for (int i = 0; i < 2000; ++i) consistent = consistent && object.contains("key" + std::to_string(i)) == (i % 2 == 1);
  CHECK(consistent);
  CHECK(object.items()[0].key() == "key1");
  CHECK(object.items()[999].key() == "key1999");

  // A copy has a working index of its own.
  Json copy = object;
  copy.set("extra", 1);
  CHECK(copy.contains("key1999"));
  CHECK(copy.contains("extra"));
  CHECK_FALSE(object.contains("extra"));
}

TEST_CASE("json: objects behave like an ordered map under random edits") {
  // Reference model: a plain vector of pairs with linear search.
  std::vector<std::pair<std::string, std::int64_t>> model;
  Json object = Json::object();
  std::uint32_t state = 2463534242u;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  const auto model_find = [&model](const std::string& key) {
    for (std::size_t i = 0; i < model.size(); ++i) {
      if (model[i].first == key) return i;
    }
    return model.size();
  };
  bool agreed = true;
  std::size_t largest = 0;
  std::size_t smallest_after_growth = 1000;
  for (int step = 0; step < 30000; ++step) {
    // Phases of growing to hundreds of members, draining again, and staying
    // small, so the object crosses the index threshold in both directions.
    const int phase = (step / 3000) % 3;
    const std::uint32_t pool = phase == 2 ? 12 : 400;
    // Shares out of 10: `set_share` set, one more inserts (not while draining), the rest erase.
    const std::uint32_t set_share = phase == 0 ? 7 : phase == 1 ? 0 : 4;
    const std::string key = "k" + std::to_string(next() % pool);
    const std::int64_t value = static_cast<std::int64_t>(next() % 1000);
    const std::size_t at = model_find(key);
    const std::uint32_t choice = next() % 10;
    if (choice < set_share) {
      object.set(key, value);
      if (at == model.size()) {
        model.emplace_back(key, value);
      } else {
        model[at].second = value;
      }
    } else if (choice == set_share && phase != 1) {
      agreed = agreed && object.insert(key, value) == (at == model.size());
      if (at == model.size()) model.emplace_back(key, value);
    } else {
      agreed = agreed && object.erase(key) == (at != model.size());
      if (at != model.size()) model.erase(model.begin() + static_cast<std::ptrdiff_t>(at));
    }
    largest = std::max(largest, model.size());
    smallest_after_growth = step > 3000 ? std::min(smallest_after_growth, model.size()) : smallest_after_growth;
    if (step % 250 == 0 || step == 29999) {
      agreed = agreed && object.size() == model.size();
      std::size_t i = 0;
      for (const auto& [member_key, member_value] : object.items()) {
        agreed = agreed && i < model.size() && member_key == model[i].first && member_value.as_int64(-1) == model[i].second;
        ++i;
      }
      for (std::uint32_t k = 0; k < 400; ++k) {
        const std::string probe = "k" + std::to_string(k);
        const std::size_t expected = model_find(probe);
        const Json* found = object.find(probe);
        agreed = agreed && (found != nullptr) == (expected != model.size());
        if (found != nullptr && expected != model.size()) agreed = agreed && found->as_int64(-1) == model[expected].second;
      }
      REQUIRE(agreed);
    }
  }
  CHECK(agreed);
  CHECK(largest > 300);              // well into indexed territory ...
  CHECK(smallest_after_growth < 8);  // ... and back down to a handful
}

TEST_CASE("json: reading through the wrong type is neutral") {
  const Json text = "text";
  CHECK(text.size() == 0);
  CHECK(text.empty());
  CHECK(text.elements().empty());
  CHECK(text.items().empty());
  CHECK(text.at(0) == nullptr);
  CHECK(text.find("key") == nullptr);
  CHECK_FALSE(text.contains("key"));
  CHECK(text[0].is_null());
  CHECK(text["key"].is_null());
  CHECK(text["a"]["b"][3]["c"].as_string().empty());  // chains stay safe
  CHECK(text["a"]["b"][3]["c"].as_int64(-7) == -7);

  const Json list = Json::array({1, 2});
  CHECK(list.find("key") == nullptr);
  CHECK(list["key"].is_null());
  CHECK(list.items().empty());
  const Json object = Json::object({{"a", 1}});
  CHECK(object.at(0) == nullptr);
  CHECK(object[0].is_null());
  CHECK(object.elements().empty());
  CHECK(object.size() == 1);

  Json scalar = 5;
  CHECK_FALSE(scalar.erase(std::size_t{0}));
  CHECK_FALSE(scalar.erase("key"));
  scalar.reserve(10);  // no effect on a non-container
  CHECK(scalar.as_int64() == 5);
  for (Json& element : scalar.elements()) element = 1;
  for (auto& [key, value] : scalar.items()) value = 1;
  CHECK(scalar.as_int64() == 5);
}

TEST_CASE("json: mutators turn other values into the container they need") {
  Json a;
  a.push_back(1);
  CHECK(dump_json(a) == "[1]");
  Json b;
  b["key"] = 1;
  CHECK(dump_json(b) == R"({"key":1})");
  Json c;
  c[2] = "x";
  CHECK(dump_json(c) == R"([null,null,"x"])");
  Json d;
  CHECK(d.insert("k", 1));
  CHECK(dump_json(d) == R"({"k":1})");
  Json e;
  e.set("k", 2);
  CHECK(dump_json(e) == R"({"k":2})");

  // An existing value of another type is replaced.
  Json f = "text";
  f["k"] = 1;
  CHECK(dump_json(f) == R"({"k":1})");
  f.push_back(2);
  CHECK(dump_json(f) == "[2]");
  f.set("again", true);
  CHECK(dump_json(f) == R"({"again":true})");
  f[1] = 5;
  CHECK(dump_json(f) == "[null,5]");

  // Arguments that alias the value being replaced are safe.
  Json g = "name";
  g.set(g.as_string(), 1);
  CHECK(dump_json(g) == R"({"name":1})");
  Json h = "name";
  h[h.as_string()] = 2;
  CHECK(dump_json(h) == R"({"name":2})");
  Json i = Json::array({1, 2});
  i.push_back(i);
  CHECK(dump_json(i) == "[1,2,[1,2]]");
  Json j = Json::object({{"k", Json::object({{"k", 1}})}});
  j.set("k", j["k"]["k"]);
  CHECK(dump_json(j) == R"({"k":1})");
}

TEST_CASE("json: copies are deep and moves leave null") {
  Json original = json(R"({"list":[1,{"deep":[true,"text"]}],"n":1.5})");
  Json copy = original;
  CHECK((copy == original));
  CHECK(dump_json(copy) == dump_json(original));
  copy["list"][1]["deep"].push_back("changed");
  copy["n"] = 2;
  CHECK(dump_json(original) == R"({"list":[1,{"deep":[true,"text"]}],"n":1.5})");
  CHECK(dump_json(copy) == R"({"list":[1,{"deep":[true,"text","changed"]}],"n":2})");

  Json moved = std::move(original);
  CHECK(original.is_null());  // NOLINT(bugprone-use-after-move): the moved-from state is specified
  CHECK(dump_json(moved) == R"({"list":[1,{"deep":[true,"text"]}],"n":1.5})");
  Json assigned = "old";
  assigned = std::move(moved);
  CHECK(moved.is_null());  // NOLINT(bugprone-use-after-move)
  CHECK(assigned.is_object());
  assigned = copy;
  CHECK((assigned == copy));

  // Self-assignment, by copy and by move.
  Json& alias = assigned;
  assigned = alias;
  CHECK((assigned == copy));
  assigned = std::move(alias);
  CHECK((assigned == copy));

  // A value can be replaced by one of its own children.
  Json tree = json(R"({"inner":{"leaf":[1,2,3]}})");
  tree = tree["inner"];
  CHECK(dump_json(tree) == R"({"leaf":[1,2,3]})");
  tree = std::move(tree["leaf"]);
  CHECK(dump_json(tree) == "[1,2,3]");
  tree = std::move(tree[2]);
  CHECK(tree.as_int64() == 3);

  Json left = Json::array({1});
  Json right = "text";
  left.swap(right);
  CHECK(left.as_string() == "text");
  CHECK(dump_json(right) == "[1]");
  swap(left, right);
  CHECK(right.as_string() == "text");

  // Strings are copied, not shared.
  Json first = "shared?";
  Json second = first;
  second = "no";
  CHECK(first.as_string() == "shared?");
}

TEST_CASE("json: equality is structural") {
  SUBCASE("numbers compare by mathematical value") {
    CHECK((Json(1) == Json(1)));
    CHECK((Json(1) == Json(1u)));
    CHECK((Json(1) == Json(1.0)));
    CHECK((Json(1u) == Json(1.0)));
    CHECK((Json(0) == Json(-0.0)));
    CHECK((Json(0.0) == Json(-0.0)));
    CHECK((Json(1) != Json(2)));
    CHECK((Json(1) != Json(1.5)));
    CHECK((Json(-1) != Json(uint64_max)));
    CHECK((Json(-1) != Json(1u)));
    CHECK((Json(int64_max) == Json(static_cast<std::uint64_t>(int64_max))));
    CHECK((Json(uint64_max) == Json(uint64_max)));
    // Exact where a conversion to double would lose the difference.
    CHECK((Json(std::int64_t{9007199254740992}) == Json(9007199254740992.0)));
    CHECK((Json(std::int64_t{9007199254740993}) != Json(9007199254740992.0)));
    CHECK((Json(std::int64_t{9007199254740993}) != Json(std::int64_t{9007199254740992})));
    CHECK((Json(uint64_max) != Json(18446744073709551616.0)));
    CHECK((Json(int64_max) != Json(9223372036854775808.0)));
    CHECK((Json(std::uint64_t{9223372036854775808ull}) == Json(9223372036854775808.0)));
    CHECK((Json(int64_min) == Json(-9223372036854775808.0)));
    CHECK((Json(1e300) != Json(int64_max)));
    // NaN equals nothing; infinities equal themselves.
    CHECK((Json(nan_value) != Json(nan_value)));
    CHECK((Json(nan_value) != Json(0)));
    CHECK((Json(infinity) == Json(infinity)));
    CHECK((Json(infinity) != Json(-infinity)));
    CHECK((Json(infinity) != Json(uint64_max)));
    const Json with_nan = Json::array({1, nan_value});
    CHECK_FALSE((with_nan == with_nan));
  }
  SUBCASE("types never mix") {
    CHECK((Json() == Json(nullptr)));
    CHECK((Json() != Json(false)));
    CHECK((Json() != Json(0)));
    CHECK((Json() != Json("")));
    CHECK((Json(true) == Json(true)));
    CHECK((Json(true) != Json(false)));
    CHECK((Json(true) != Json(1)));
    CHECK((Json("1") != Json(1)));
    CHECK((Json("a") == Json("a")));
    CHECK((Json("a") != Json("A")));
    CHECK((Json("a") != Json("a\0"sv)));
    CHECK((Json::array() != Json::object()));
    CHECK((Json::array() != Json()));
    CHECK((Json::array() == Json::array()));
    CHECK((Json::object() == Json::object()));
    // Comparing against plain C++ values goes through the converting constructors.
    CHECK((Json(5) == 5));
    CHECK((Json("x") == "x"));
    CHECK((Json(2.5) != 2));
    CHECK((true == Json(true)));
  }
  SUBCASE("arrays are ordered") {
    CHECK((json("[1,2,3]") == json("[1, 2.0, 3]")));
    CHECK((json("[1,2,3]") != json("[3,2,1]")));
    CHECK((json("[1,2,3]") != json("[1,2]")));
    CHECK((json("[[1],[2,[3]]]") == json("[[1],[2,[3]]]")));
    CHECK((json("[[1],[2,[3]]]") != json("[[1],[2,[4]]]")));
    CHECK((json("[[]]") != json("[{}]")));
  }
  SUBCASE("object member order is ignored by == and honoured by equal_ordered") {
    const Json a = json(R"({"x":1,"y":{"p":[1,2],"q":null},"z":"s"})");
    const Json b = json(R"({"z":"s","y":{"q":null,"p":[1,2]},"x":1.0})");
    CHECK((a == b));
    CHECK((b == a));
    CHECK_FALSE(a.equal_ordered(b));
    CHECK(a.equal_ordered(a));
    CHECK(a.equal_ordered(json(R"({"x":1.0,"y":{"p":[1,2],"q":null},"z":"s"})")));
    CHECK_FALSE(a.equal_ordered(json(R"({"x":1,"y":{"q":null,"p":[1,2]},"z":"s"})")));  // nested order counts too
    CHECK((json(R"({"a":1})") != json(R"({"a":2})")));
    CHECK((json(R"({"a":1})") != json(R"({"b":1})")));
    CHECK((json(R"({"a":1})") != json(R"({"a":1,"b":2})")));
    CHECK((json(R"({"a":null})") != json("{}")));
    CHECK((json(R"({"a":1,"b":2})") != json(R"({"a":1,"c":2})")));
    CHECK((json(R"({"a":[1,2]})") != json(R"({"a":[2,1]})")));

    // Large (indexed) objects in opposite orders.
    Json forward = Json::object();
    Json backward = Json::object();
    for (int i = 0; i < 300; ++i) forward.set("k" + std::to_string(i), i);
    for (int i = 299; i >= 0; --i) backward.set("k" + std::to_string(i), static_cast<double>(i));
    CHECK((forward == backward));
    CHECK_FALSE(forward.equal_ordered(backward));
    backward["k150"] = -1;
    CHECK((forward != backward));
  }
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

TEST_CASE("json: number literals are classified by value") {
  struct Case {
    std::string_view text;
    Json::Type type;
    std::string_view dumped;
  };
  const Case cases[] = {
      {"0", Json::Type::Int64, "0"},
      {"-0", Json::Type::Int64, "0"},
      {"7", Json::Type::Int64, "7"},
      {"-7", Json::Type::Int64, "-7"},
      {"9007199254740993", Json::Type::Int64, "9007199254740993"},
      {"9223372036854775807", Json::Type::Int64, "9223372036854775807"},
      {"-9223372036854775808", Json::Type::Int64, "-9223372036854775808"},
      {"9223372036854775808", Json::Type::UInt64, "9223372036854775808"},
      {"18446744073709551615", Json::Type::UInt64, "18446744073709551615"},
      // Beyond 64 bits an integer literal becomes the nearest double.
      {"18446744073709551616", Json::Type::Double, "1.8446744073709552e+19"},
      {"-9223372036854775809", Json::Type::Double, "-9.223372036854776e+18"},
      {"100000000000000000000", Json::Type::Double, "1e+20"},
      {"123456789012345678901234567890", Json::Type::Double, "1.2345678901234568e+29"},
      // A fraction or an exponent always makes a double.
      {"0.0", Json::Type::Double, "0.0"},
      {"-0.0", Json::Type::Double, "-0.0"},
      {"-0e0", Json::Type::Double, "-0.0"},
      {"1.0", Json::Type::Double, "1.0"},
      {"1e0", Json::Type::Double, "1.0"},
      {"1E2", Json::Type::Double, "100.0"},
      {"1e5", Json::Type::Double, "100000.0"},
      {"0.1e1", Json::Type::Double, "1.0"},
      {"1.5e+3", Json::Type::Double, "1500.0"},
      {"12.50", Json::Type::Double, "12.5"},
      {"9007199254740993.0", Json::Type::Double, "9007199254740992.0"},
      // Correct rounding at the edges of the double range.
      {"1.7976931348623157e308", Json::Type::Double, "1.7976931348623157e+308"},
      {"1.7976931348623158e308", Json::Type::Double, "1.7976931348623157e+308"},
      {"2.2250738585072014e-308", Json::Type::Double, "2.2250738585072014e-308"},
      {"5e-324", Json::Type::Double, "5e-324"},
      {"4.9406564584124654e-324", Json::Type::Double, "5e-324"},
      {"2.4703282292062328e-324", Json::Type::Double, "5e-324"},
      {"2.4703282292062327e-324", Json::Type::Double, "0.0"},
      {"1e-400", Json::Type::Double, "0.0"},
      {"-1e-400", Json::Type::Double, "-0.0"},
      {"0e999999999999999999999", Json::Type::Double, "0.0"},
      {"0.1", Json::Type::Double, "0.1"},
      {"0.30000000000000004", Json::Type::Double, "0.30000000000000004"},
      {"0.3000000000000000444089209850062616169452667236328125", Json::Type::Double, "0.30000000000000004"},
      {"1.00000000000000011102230246251565404236316680908203125", Json::Type::Double, "1.0"},  // exactly halfway: ties to even
      {"1.00000000000000011102230246251565404236316680908203126", Json::Type::Double, "1.0000000000000002"},
      {"123456789012345678.9e-3", Json::Type::Double, "123456789012345.67"},
      {"0.000000000000000000000000000001e30", Json::Type::Double, "1.0"},
      {"1000000000000000000000000000000e-30", Json::Type::Double, "1.0"},
  };
  for (const Case& c : cases) {
    const auto parsed = parse_json(c.text);
    CHECK_MESSAGE(parsed.has_value(), "should parse: ", c.text);
    if (!parsed) continue;
    CHECK_MESSAGE(parsed->type() == c.type, "type of ", c.text);
    CHECK_MESSAGE(dump_json(*parsed) == c.dumped, "dump of ", c.text, " gave ", dump_json(*parsed));
  }
  CHECK(parse_json("9007199254740993")->as_int64() == 9007199254740993);
  CHECK(parse_json("18446744073709551615")->as_uint64() == uint64_max);
  CHECK(parse_json("-9223372036854775808")->as_int64() == int64_min);
  CHECK(std::signbit(parse_json("-0.0")->as_double()));
  CHECK_FALSE(parse_json("-0")->is_float());
}

TEST_CASE("json: doubles are written in their shortest round-trip form") {
  struct Case {
    double value;
    std::string_view text;
  };
  const Case cases[] = {
      {0.0, "0.0"},
      {-0.0, "-0.0"},
      {1.0, "1.0"},
      {-1.0, "-1.0"},
      {0.1, "0.1"},
      {0.5, "0.5"},
      {1.5, "1.5"},
      {100.0, "100.0"},
      {1234.5, "1234.5"},
      {123456.789, "123456.789"},
      {12345678.9, "12345678.9"},
      {4.35, "4.35"},
      {0.1 + 0.7, "0.7999999999999999"},
      {0.1 + 0.2, "0.30000000000000004"},
      {1.0 / 3.0, "0.3333333333333333"},
      {2.0 / 3.0, "0.6666666666666666"},
      {3.141592653589793, "3.141592653589793"},
      {static_cast<double>(0.1f), "0.10000000149011612"},
      // Positional notation up to (not including) 1e16 ...
      {1e15, "1000000000000000.0"},
      {1000000000000000.5, "1000000000000000.5"},
      {9007199254740992.0, "9007199254740992.0"},
      {9999999999999998.0, "9999999999999998.0"},
      // ... exponent notation from there on.
      {1e16, "1e+16"},
      {1.5e16, "1.5e+16"},
      {123456789012345680.0, "1.2345678901234568e+17"},
      {1e21, "1e+21"},
      {1e22, "1e+22"},
      {1e23, "1e+23"},
      {1e100, "1e+100"},
      {1.7976931348623157e308, "1.7976931348623157e+308"},
      {-1.7976931348623157e308, "-1.7976931348623157e+308"},
      // Positional notation down to 1e-4, exponent notation below.
      {0.001, "0.001"},
      {0.0001, "0.0001"},
      {0.000123, "0.000123"},
      {0.00001, "1e-05"},
      {9.999e-5, "9.999e-05"},
      {2.5e-5, "2.5e-05"},
      {1e-7, "1e-07"},
      {1.5e-7, "1.5e-07"},
      {1.23e-18, "1.23e-18"},
      {2.2250738585072014e-308, "2.2250738585072014e-308"},
      {5e-324, "5e-324"},
      {-5e-324, "-5e-324"},
  };
  for (const Case& c : cases) {
    const std::string text = dump_json(Json(c.value));
    CHECK_MESSAGE(text == c.text, "expected ", c.text, " but got ", text);
    // Always a double again, and always the identical one.
    const auto back = parse_json(text);
    REQUIRE(back.has_value());
    CHECK_MESSAGE(back->is_float(), text, " should read back as a double");
    CHECK_MESSAGE(back->as_double() == c.value, text, " should read back to the same value");
    CHECK(std::signbit(back->as_double()) == std::signbit(c.value));
  }

  // Integers are exact, whatever their kind.
  CHECK(dump_json(Json(int64_min)) == "-9223372036854775808");
  CHECK(dump_json(Json(int64_max)) == "9223372036854775807");
  CHECK(dump_json(Json(uint64_max)) == "18446744073709551615");
  CHECK(dump_json(Json(0u)) == "0");
  // Non-finite doubles have no JSON spelling.
  CHECK(dump_json(Json::array({nan_value, infinity, -infinity})) == "[null,null,null]");

  // Every double survives text, including awkward bit patterns.
  std::uint64_t bits = 0x9E3779B97F4A7C15ull;
  bool round_trips = true;
  for (int i = 0; i < 20000; ++i) {
    bits ^= bits << 13;
    bits ^= bits >> 7;
    bits ^= bits << 17;
    double value = 0;
    static_assert(sizeof(value) == sizeof(bits));
    ::cvt::util::copy_bytes(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) continue;
    const auto back = parse_json(dump_json(Json(value)));
    round_trips = round_trips && back && back->is_float() && back->as_double() == value;
  }
  CHECK(round_trips);
}

TEST_CASE("json: the number grammar is exact") {
  for (const std::string_view good : {"0", "-0", "0.0", "-0.0", "1", "-1", "10", "123", "1.5", "0.5", "1e1", "1E1", "1e+1", "1e-1", "1.5e10",
                                      "0e0", "0E-0", "-0.0e+0", "1e00", "1e-00", "1e010", "0.000", "12345678901234567890", "1.0e-10",
                                      "-123.456e-78"}) {
    CHECK_MESSAGE(accepts(good), "should accept: ", good);
  }
  for (const std::string_view bad : {"+1", "+0", "01", "-01", "00", "-00", "1.", "-1.", ".1", "-.1", "1.e1", "1e", "1E", "1e+", "1e-", "1e1.5",
                                     "0x10", "0X1F", "1_000", "1,000", "--1", "-", "- 1", "1 e1", "1e 1", "Infinity", "-Infinity", "NaN",
                                     "inf", "nan", "1f", "1d", "1.0.0", "0e", "0.e1", "1..2", "1ee1", "1e1e1", "1e++1", "0b11", "1L", "1u",
                                     "\xD9\xA1", "\xEF\xBC\x91"}) {
    CHECK_MESSAGE(!accepts(bad), "should reject: ", bad);
  }
  // Too large for a double is an error, in either direction of the sign.
  CHECK(reparse("1e999") == "error: number out of range");
  CHECK(reparse("-1e999") == "error: number out of range");
  CHECK(reparse("1E400") == "error: number out of range");
  CHECK(reparse("1.7976931348623159e308") == "error: number out of range");
  CHECK(reparse("1" + std::string(400, '0')) == "error: number out of range");
  CHECK(reparse("1" + std::string(400, '0') + ".5") == "error: number out of range");
  // Too small is zero.
  CHECK(reparse("1e-999") == "0.0");
  CHECK(reparse("0." + std::string(400, '0') + "1") == "0.0");
  // Long but ordinary.
  CHECK(reparse("0." + std::string(400, '0') + "1e401") == "1.0");
  CHECK(reparse("1" + std::string(400, '0') + "e-400") == "1.0");
  CHECK(reparse("0.1234567890123456789012345678901234567890") == "0.12345678901234568");
  // Whitespace does not belong inside a number.
  CHECK(reparse("[1 , -2\t,\n3e1 ]") == "[1,-2,30.0]");
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

TEST_CASE("json: every string escape is read and written") {
  // Reading: the eight short escapes and \u.
  const auto parsed = parse_json(R"("\" \\ \/ \b \f \n \r \t \u0041 \u00e9 \u00E9 \u20ac \u0000")");
  REQUIRE(parsed.has_value());
  CHECK(parsed->as_string() == "\" \\ / \b \f \n \r \t A \xC3\xA9 \xC3\xA9 \xE2\x82\xAC \0"sv);
  CHECK(parse_json(R"("\uFFFF\ufffd\u007f\u0080\u07ff\u0800")")->as_string() ==
        "\xEF\xBF\xBF\xEF\xBF\xBD\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80");
  CHECK(parse_json("\"\"")->as_string().empty());
  CHECK(parse_json("\"'single' / slash\"")->as_string() == "'single' / slash");
  CHECK(parse_json("\"\x7F\"")->as_string() == "\x7F");  // DEL needs no escape

  // Writing: quotes, backslash and the control characters; nothing else.
  CHECK(dump_json(Json("\" \\ / \b \f \n \r \t")) == R"("\" \\ / \b \f \n \r \t")");
  std::string controls;
  for (char c = 0; c < 0x20; ++c) controls.push_back(c);
  CHECK(dump_json(Json(controls)) ==
        R"("\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f)"
        R"(\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d\u001e\u001f")");
  CHECK(dump_json(Json("\x7F caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \xE2\x80\xA8")) ==
        "\"\x7F caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \xE2\x80\xA8\"");
  CHECK(dump_json(Json("")) == "\"\"");
  // Keys are escaped like any other string.
  CHECK(dump_json(Json::object({{"a\"b\n", 1}})) == R"({"a\"b\n":1})");

  // Any byte string survives a round trip when it is valid UTF-8.
  std::string everything;
  for (char32_t cp = 0; cp < 0x3000; ++cp) {
    if (cp >= 0xD800 && cp <= 0xDFFF) continue;
    append_utf8(everything, cp);
  }
  append_utf8(everything, 0xFFFF);
  append_utf8(everything, 0x10000);
  append_utf8(everything, 0x10FFFF);
  for (const bool ascii_only : {false, true}) {
    const auto back = parse_json(dump_json(Json(everything), JsonWriteOptions{.ascii_only = ascii_only}));
    REQUIRE(back.has_value());
    CHECK(back->as_string() == everything);
  }

  for (const std::string_view bad : {R"("\a")", R"("\v")", R"("\0")", R"("\x41")", R"("\')", R"("\ ")", R"("\U0041")", R"("\u")", R"("\u1")",
                                     R"("\u12")", R"("\u123")", R"("\u123G")", R"("\uZZZZ")", R"("\u+123")", R"("\u 123")", R"("\u00)",
                                     R"("\)", R"("abc\)", "\"\\\n\""}) {
    CHECK_MESSAGE(!accepts(bad), "should reject: ", bad);
  }
  CHECK(reparse(R"("\q")") == "error: invalid escape");
  CHECK(reparse(R"("\u12G4")") == "error: invalid \\u escape");
  CHECK(reparse(R"("\u12)") == "error: invalid \\u escape");

  // Raw control characters must be escaped.
  for (char c = 0; c < 0x20; ++c) {
    const std::string text = std::string("\"a") + c + "b\"";
    CHECK_MESSAGE(reparse(text) == "error: control character in string", "control character ", static_cast<int>(c));
  }
}

TEST_CASE("json: surrogate pairs") {
  CHECK(parse_json(R"("\ud83d\ude00")")->as_string() == "\xF0\x9F\x98\x80");
  CHECK(parse_json(R"("\uD83D\uDE00")")->as_string() == "\xF0\x9F\x98\x80");
  CHECK(parse_json(R"("\ud800\udc00")")->as_string() == "\xF0\x90\x80\x80");  // U+10000
  CHECK(parse_json(R"("\udbff\udfff")")->as_string() == "\xF4\x8F\xBF\xBF");  // U+10FFFF
  CHECK(parse_json(R"("a\ud83d\ude00b\ud83d\ude00")")->as_string() == "a\xF0\x9F\x98\x80" "b\xF0\x9F\x98\x80");

  for (const std::string_view lone : {R"("\ud800")", R"("\udbff")", R"("\udc00")", R"("\udfff")", R"("\ud83d")", R"("\ud83dx")",
                                      R"("\ud83d\n")", R"("\ud83d\u0041")", R"("\ud83d\ud83d")", R"("\ude00\ud83d")", R"("\ud83d\\ude00")",
                                      R"("\ud83d \ude00")", R"("\ud83d\ude0")", R"("\ud83d\u")"}) {
    CHECK_MESSAGE(reparse(lone).starts_with("error: "), "should reject: ", lone);
  }
  CHECK(reparse(R"("\ud800")") == "error: lone surrogate");
  CHECK(reparse(R"("\udc00")") == "error: lone surrogate");
  CHECK(reparse(R"("x\ud83d\u0041")") == "error: lone surrogate");

  // Written back as UTF-8, or as a pair of escapes in ASCII mode.
  const Json face = "\xF0\x9F\x98\x80";
  CHECK(dump_json(face) == "\"\xF0\x9F\x98\x80\"");
  CHECK(dump_json(face, JsonWriteOptions{.ascii_only = true}) == R"("\ud83d\ude00")");
  CHECK(dump_json(Json("\xF0\x90\x80\x80\xF4\x8F\xBF\xBF"), JsonWriteOptions{.ascii_only = true}) == R"("\ud800\udc00\udbff\udfff")");
}

TEST_CASE("json: input must be valid UTF-8") {
  for (const std::string_view good : {"\"\xC2\x80\"", "\"\xDF\xBF\"", "\"\xE0\xA0\x80\"", "\"\xED\x9F\xBF\"", "\"\xEE\x80\x80\"",
                                      "\"\xEF\xBF\xBF\"", "\"\xF0\x90\x80\x80\"", "\"\xF4\x8F\xBF\xBF\"",
                                      "{\"\xE6\x97\xA5\xE6\x9C\xAC\":\"\xE8\xAA\x9E\"}"}) {
    CHECK_MESSAGE(accepts(good), "should accept: ", good);
  }
  for (const std::string_view bad : {"\"\x80\"", "\"\xBF\"", "\"\xC0\x80\"", "\"\xC1\xBF\"", "\"\xC3\"", "\"\xC3\x28\"", "\"\xE0\x80\x80\"",
                                     "\"\xE0\x9F\xBF\"", "\"\xE2\x82\"", "\"\xED\xA0\x80\"", "\"\xED\xBF\xBF\"", "\"\xF0\x80\x80\x80\"",
                                     "\"\xF0\x8F\xBF\xBF\"", "\"\xF0\x9F\x98\"", "\"\xF4\x90\x80\x80\"", "\"\xF5\x80\x80\x80\"",
                                     "\"\xF8\x88\x80\x80\x80\"", "\"\xFE\"", "\"\xFF\"", "\"a\xFFz\""}) {
    CHECK_MESSAGE(reparse(bad) == "error: invalid UTF-8", "should reject: ", bad);
  }
  // Outside strings a stray byte is simply not JSON.
  CHECK_FALSE(accepts("\xFF"));
  CHECK_FALSE(accepts("[1,\xC3\xA9]"));
  CHECK_FALSE(accepts("\xC2\xA0" "1"));  // no-break space is not whitespace
  // Other encodings are not guessed.
  CHECK_FALSE(accepts("\xFF\xFE[\0]\0"sv));
  CHECK_FALSE(accepts("[\0]\0"sv));
  // A byte order mark is tolerated once, at the very start.
  CHECK(reparse("\xEF\xBB\xBF{\"a\":1}") == R"({"a":1})");
  CHECK(reparse("\xEF\xBB\xBF 1 ") == "1");
  CHECK_FALSE(accepts("\xEF\xBB\xBF"));
  CHECK_FALSE(accepts("\xEF\xBB\xBF\xEF\xBB\xBF" "1"));
  CHECK_FALSE(accepts("1\xEF\xBB\xBF"));
  CHECK_FALSE(accepts(" \xEF\xBB\xBF" "1"));
  CHECK(parse_json("\"\xEF\xBB\xBF\"")->as_string() == "\xEF\xBB\xBF");  // inside a string it is just U+FEFF

  // The serialiser repairs what values built in code may contain.
  CHECK(dump_json(Json("\xC3")) == "\"\xEF\xBF\xBD\"");
  CHECK(dump_json(Json("a\xF0\x9F\x98z")) == "\"a\xEF\xBF\xBDz\"");  // one replacement per truncated sequence
  CHECK(dump_json(Json("\xED\xA0\x80")) == "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"");
  CHECK(dump_json(Json::object({{"k\xFF", "v\x80"}})) == "{\"k\xEF\xBF\xBD\":\"v\xEF\xBF\xBD\"}");
  CHECK(dump_json(Json("a\xFFz"), JsonWriteOptions{.ascii_only = true}) == R"("a\ufffdz")");
  CHECK(accepts(dump_json(Json(std::string("\x80\xBF\xC0\xFF\xED\xA0\x80\xF4\x90", 9)))));
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST_CASE("json: errors carry line, column, byte offset and a reason") {
  struct Case {
    std::string_view text;
    std::size_t line;
    std::size_t column;
    std::size_t offset;
    std::string_view reason;
  };
  const Case cases[] = {
      // End of input.
      {"", 1, 1, 0, "unexpected end of input"},
      {"   ", 1, 4, 3, "unexpected end of input"},
      {"\n\n", 3, 1, 2, "unexpected end of input"},
      {"[", 1, 2, 1, "unexpected end of input"},
      {"[1", 1, 3, 2, "unexpected end of input"},
      {"[1,", 1, 4, 3, "unexpected end of input"},
      {"{", 1, 2, 1, "unexpected end of input"},
      {"{\"a\"", 1, 5, 4, "unexpected end of input"},
      {"{\"a\":", 1, 6, 5, "unexpected end of input"},
      {"{\"a\":1", 1, 7, 6, "unexpected end of input"},
      {"{\"a\":1,", 1, 8, 7, "unexpected end of input"},
      // Structure.
      {"]", 1, 1, 0, "unexpected character"},
      {"[1,]", 1, 4, 3, "unexpected character"},
      {"[,1]", 1, 2, 1, "unexpected character"},
      {"[1,,2]", 1, 4, 3, "unexpected character"},
      {"{\"a\":}", 1, 6, 5, "unexpected character"},
      {"[1,\n 2,\n x]", 3, 2, 9, "unexpected character"},
      {"[1 2]", 1, 4, 3, "expected ',' or ']'"},
      {"[1}", 1, 3, 2, "expected ',' or ']'"},
      {"[\"a\":1]", 1, 5, 4, "expected ',' or ']'"},
      {"{\"a\":1 \"b\":2}", 1, 8, 7, "expected ',' or '}'"},
      {"{\"a\":1]", 1, 7, 6, "expected ',' or '}'"},
      {"{\"a\":1:2}", 1, 7, 6, "expected ',' or '}'"},
      {"{\"a\" 1}", 1, 6, 5, "expected ':'"},
      {"{\"a\"}", 1, 5, 4, "expected ':'"},
      {"{\"a\",1}", 1, 5, 4, "expected ':'"},
      {"{1:2}", 1, 2, 1, "expected a string key"},
      {"{\"a\":1,}", 1, 8, 7, "expected a string key"},
      {"{,}", 1, 2, 1, "expected a string key"},
      {"{null:1}", 1, 2, 1, "expected a string key"},
      {"{\"a\":1,\n  b:2}", 2, 3, 10, "expected a string key"},
      // Literals.
      {"nul", 1, 1, 0, "invalid literal"},
      {"[tru]", 1, 2, 1, "invalid literal"},
      {"[true, fals", 1, 8, 7, "invalid literal"},
      {"TRUE", 1, 1, 0, "unexpected character"},
      {"undefined", 1, 1, 0, "unexpected character"},
      {"NaN", 1, 1, 0, "unexpected character"},
      {"'x'", 1, 1, 0, "unexpected character"},
      // Strings.
      {"\"abc", 1, 1, 0, "unterminated string"},
      {"[\"abc\\", 1, 2, 1, "unterminated string"},
      {"{\"key", 1, 2, 1, "unterminated string"},
      {"\"a\\qb\"", 1, 3, 2, "invalid escape"},
      {"\"\\u12G4\"", 1, 2, 1, "invalid \\u escape"},
      {"\"ab\\ud800\"", 1, 4, 3, "lone surrogate"},
      {"\"a\tb\"", 1, 3, 2, "control character in string"},
      {"\"a\nb\"", 1, 3, 2, "control character in string"},
      {"\"\xC3\xA9\xFF\"", 1, 3, 3, "invalid UTF-8"},
      {"[\"\xE6\x97\xA5\xE6\x9C\xAC\", \"\xF0\x9F\x98\x80\xE2\x82\"]", 1, 10, 16, "invalid UTF-8"},
      // Numbers.
      {"-", 1, 1, 0, "invalid number"},
      {"[-a]", 1, 2, 1, "invalid number"},
      {"1.", 1, 1, 0, "invalid number"},
      {"1.e5", 1, 1, 0, "invalid number"},
      {"1e", 1, 1, 0, "invalid number"},
      {"1e+", 1, 1, 0, "invalid number"},
      {"[\"\xE6\x97\xA5\xE6\x9C\xAC\", 01]", 1, 8, 11, "invalid number"},
      {".5", 1, 1, 0, "unexpected character"},
      {"+1", 1, 1, 0, "unexpected character"},
      {"1e999", 1, 1, 0, "number out of range"},
      {"[0, -1e999]", 1, 5, 4, "number out of range"},
      // After the value.
      {"1 2", 1, 3, 2, "trailing characters"},
      {"{}x", 1, 3, 2, "trailing characters"},
      {"[]]", 1, 3, 2, "trailing characters"},
      {"null\n\n  ,", 3, 3, 8, "trailing characters"},
      {"1.5.2", 1, 4, 3, "trailing characters"},
      {"\"a\" \"b\"", 1, 5, 4, "trailing characters"},
      // Lines end at LF, CRLF or a lone CR; columns count code points; a BOM
      // shifts offsets but not columns.
      {"[1,\r\n2,\r3,\nx]", 4, 1, 11, "unexpected character"},
      {"[\r\n\r\n  !", 3, 3, 7, "unexpected character"},
      {"\xEF\xBB\xBF x", 1, 2, 4, "unexpected character"},
      {"\xEF\xBB\xBF[\n\"\xC3\xA9\" x", 2, 5, 10, "expected ',' or ']'"},
  };
  for (const Case& c : cases) {
    JsonLocation where;
    const auto result = parse_json(c.text, JsonParseOptions{}, &where);
    CHECK_MESSAGE(!result.has_value(), "should reject: ", c.text);
    if (result) continue;
    CHECK(result.error().code == Status::Decode);
    CHECK_MESSAGE(where.line == c.line, "line for: ", c.text, " -> ", where.line);
    CHECK_MESSAGE(where.column == c.column, "column for: ", c.text, " -> ", where.column);
    CHECK_MESSAGE(where.offset == c.offset, "offset for: ", c.text, " -> ", where.offset);
    const std::string expected = "invalid JSON at line " + std::to_string(c.line) + ", column " + std::to_string(c.column) + " (byte " +
                                 std::to_string(c.offset) + "): " + std::string(c.reason);
    CHECK_MESSAGE(result.error().message == expected, "message for: ", c.text, " -> ", result.error().message);
    // The short overload reports the same thing.
    const auto plain = parse_json(c.text);
    CHECK((!plain && plain.error().message == expected));
  }

  // The location is left alone on success and optional on failure.
  JsonLocation untouched{.offset = 77, .line = 88, .column = 99};
  CHECK(parse_json("[1]", JsonParseOptions{}, &untouched).has_value());
  CHECK((untouched.offset == 77 && untouched.line == 88 && untouched.column == 99));
  CHECK_FALSE(parse_json("[1", JsonParseOptions{}, nullptr));

  // Errors specific to options.
  JsonLocation where;
  CHECK_FALSE(parse_json("[[1]]", JsonParseOptions{.max_depth = 1}, &where));
  CHECK((where.offset == 1 && where.line == 1 && where.column == 2));
  CHECK(reparse("[[1]]", JsonParseOptions{.max_depth = 1}) == "error: nesting too deep (limit 1)");
  CHECK_FALSE(parse_json(R"({"a":1,"b":2,"a":3})", JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::Reject}, &where));
  CHECK((where.offset == 13 && where.line == 1 && where.column == 14));
  CHECK_FALSE(parse_json("[1, /* x", JsonParseOptions{.allow_comments = true}, &where));
  CHECK((where.offset == 4 && where.line == 1 && where.column == 5));
  CHECK(reparse("[1, /* x", JsonParseOptions{.allow_comments = true}) == "error: unterminated comment");
}

TEST_CASE("json: strict mode accepts exactly RFC 8259") {
  const std::pair<std::string_view, std::string_view> accepted[] = {
      {"null", "null"},
      {"true", "true"},
      {"false", "false"},
      {"0", "0"},
      {"\"\"", "\"\""},
      {"[]", "[]"},
      {"{}", "{}"},
      {" \t\r\n[ \t\r\n] \t\r\n", "[]"},
      {"{ }", "{}"},
      {"[[]]", "[[]]"},
      {"[{}]", "[{}]"},
      {"{\"\":{}}", "{\"\":{}}"},
      {"[null,true,false,0,\"\",[],{}]", "[null,true,false,0,\"\",[],{}]"},
      {" [ 1 , 2 , [ 3 , { \"a\" : [ ] } ] ] ", "[1,2,[3,{\"a\":[]}]]"},
      {"{\"a\":{\"b\":{\"c\":[1,[2,[3]]]}}}", "{\"a\":{\"b\":{\"c\":[1,[2,[3]]]}}}"},
      {"{\n\t\"key\" :\r\n\"value\"\n}", "{\"key\":\"value\"}"},
      {"\"a b\"", "\"a b\""},
      {"\"//not a comment /* really */\"", "\"//not a comment /* really */\""},
      {"[\"[\",\"]\",\"{\",\"}\",\",\",\":\"]", "[\"[\",\"]\",\"{\",\"}\",\",\",\":\"]"},
      {"{\"true\":true,\"null\":null,\"1\":1}", "{\"true\":true,\"null\":null,\"1\":1}"},
      {"[1E2,1e-2,-0.5]", "[100.0,0.01,-0.5]"},
  };
  for (const auto& [text, expected] : accepted) {
    CHECK_MESSAGE(reparse(text) == expected, "input: ", text, " -> ", reparse(text));
  }
  for (const std::string_view rejected :
       {// nothing, or more than one value
        "", " ", "\n", "1 2", "[] []", "{}{}", "null null", "[1],", "1,", ",1", "\"a\"\"b\"",
        // unbalanced or mismatched brackets
        "[", "]", "{", "}", "[[]", "[]]", "{{}", "{}}", "[}", "{]", "[1}", "{\"a\":1]", "[{]}", "{\"a\":[}",
        // commas and colons in the wrong places
        "[,]", "[1,]", "[,1]", "[1,,2]", "[1 2]", "[1:2]", "{,}", "{\"a\":1,}", "{,\"a\":1}", "{\"a\":1,,\"b\":2}", "{\"a\"}", "{\"a\":}",
        "{:1}", "{\"a\" 1}", "{\"a\"::1}", "{\"a\":1:2}", "{\"a\":1 \"b\":2}", "{\"a\",\"b\"}", "[\"a\":1]", ":", ",",
        // keys must be strings
        "{a:1}", "{1:1}", "{null:1}", "{true:1}", "{[]:1}", "{'a':1}",
        // other languages' literals
        "TRUE", "True", "FALSE", "Null", "NULL", "nil", "None", "undefined", "NaN", "Infinity", "-Infinity", "yes", "tru", "fals", "nul",
        "truee", "nulll", "t", "n",
        // strings
        "'single'", "\"unterminated", "\"", "\"\\\"", "\"line\nbreak\"", "\"tab\there\"", "abc", "\"a\" b",
        // comments are not JSON
        "// c\n1", "/* c */ 1", "1 // c", "1 /* c */", "# c\n1", "[1, /* c */ 2]",
        // whitespace is only space, tab, LF, CR
        "\f1", "\v1", "\xC2\xA0" "1", "\xE2\x80\x8B" "1", "1\f",
        // misc
        "\\", "@", "$", "`1`", "(1)", "<1>", "1;", "[1;2]", "{\"a\"=1}", "[\"a\" \"b\"]", "[1 true]", "[*]"}) {
    CHECK_MESSAGE(!accepts(rejected), "should reject: ", rejected);
  }
  CHECK_FALSE(accepts("\0"sv));
  CHECK_FALSE(accepts("1\0"sv));
  CHECK_FALSE(accepts("[\"a\0b\"]"sv));
  CHECK_FALSE(accepts("nu\0ll"sv));

  // Every truncation of a valid document is rejected (never a crash, never a
  // silent partial value).
  const std::string_view document =
      R"({"name":"caf\u00e9 \ud83d\ude00","list":[1,-2.5e3,true,false,null,{"k":[]}],"nested":{"a":{"b":"c\\d\"e"}},"n":18446744073709551615})";
  REQUIRE(accepts(document));
  for (std::size_t length = 0; length < document.size(); ++length) {
    CHECK_MESSAGE(!accepts(document.substr(0, length)), "prefix of length ", length);
  }
}

TEST_CASE("json: duplicate keys") {
  const std::string_view text = R"({"a":1,"b":2,"a":3,"c":{"a":4,"a":5},"b":[6]})";
  // Default: the key keeps its first position and takes the last value.
  CHECK(reparse(text) == R"({"a":3,"b":[6],"c":{"a":5}})");
  CHECK(reparse(text, JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::LastWins}) == R"({"a":3,"b":[6],"c":{"a":5}})");
  CHECK(reparse(text, JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::FirstWins}) == R"({"a":1,"b":2,"c":{"a":4}})");
  CHECK(reparse(text, JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::Reject}) == "error: duplicate key");
  // The same key in different objects is not a duplicate.
  CHECK(reparse(R"([{"a":1},{"a":2},{"a":{"a":3}}])", JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::Reject}) ==
        R"([{"a":1},{"a":2},{"a":{"a":3}}])");
  // Keys are compared after unescaping.
  CHECK(reparse(R"({"a":1,"\u0061":2})") == R"({"a":2})");
  CHECK(reparse(R"({"a":1,"\u0061":2})", JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::Reject}) == "error: duplicate key");
  CHECK(reparse(R"({"a":1,"A":2,"":3,"a ":4})", JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::Reject}) ==
        R"({"a":1,"A":2,"":3,"a ":4})");

  // The same rules through the index of a large object.
  std::string large = "{";
  for (int i = 0; i < 200; ++i) large += "\"k" + std::to_string(i) + "\":" + std::to_string(i) + ",";
  large += "\"k7\":\"again\",\"k199\":\"last\"}";
  const auto last = parse_json(large);
  REQUIRE(last.has_value());
  CHECK(last->size() == 200);
  CHECK((*last)["k7"].as_string() == "again");
  CHECK(last->items()[7].key() == "k7");
  CHECK((*last)["k199"].as_string() == "last");
  const auto first = parse_json(large, JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::FirstWins});
  REQUIRE(first.has_value());
  CHECK(first->size() == 200);
  CHECK((*first)["k7"].as_int64() == 7);
  CHECK_FALSE(accepts(large, JsonParseOptions{.duplicate_keys = JsonDuplicateKeys::Reject}));
}

// ---------------------------------------------------------------------------
// Lenient parsing
// ---------------------------------------------------------------------------

TEST_CASE("json: lenient options each admit exactly one extension") {
  SUBCASE("comments") {
    const JsonParseOptions options{.allow_comments = true};
    CHECK(reparse("// leading\n[1, /* two */ 2] // trailing", options) == "[1,2]");
    CHECK(reparse("/**/1/**/", options) == "1");
    CHECK(reparse("/* a */ /* b */\n// c\r\n{ /* k */ \"a\" /* colon */ : /* v */ 1 /* end */ }", options) == R"({"a":1})");
    CHECK(reparse("[1,// x\r2]", options) == "[1,2]");  // a lone CR ends a line comment
    CHECK(reparse("1 // no newline at the end", options) == "1");
    CHECK(reparse("/* multi\nline * / ** */ 1", options) == "1");
    CHECK(reparse("\"a // b /* c */\"", options) == "\"a // b /* c */\"");  // not inside strings
    CHECK(reparse("// caf\xC3\xA9\n1", options) == "1");
    CHECK(reparse("1 /* open", options) == "error: unterminated comment");
    CHECK(reparse("/*/ 1", options) == "error: unterminated comment");
    CHECK(reparse("/ 1", options) == "error: unexpected character");
    CHECK(reparse("1 /", options) == "error: trailing characters");
    CHECK(reparse("/* a /* b */ */ 1", options) == "error: unexpected character");  // comments do not nest
    CHECK(reparse("# c\n1", options) == "error: unexpected character");
    CHECK(reparse("// only a comment", options) == "error: unexpected end of input");
    CHECK(reparse("// bad \xFF\n1", options) == "error: invalid UTF-8");
    CHECK(reparse("/* bad \xC3 */1", options) == "error: invalid UTF-8");
    CHECK(reparse("// bad \xFF\n1", JsonParseOptions{.allow_comments = true, .replace_invalid_utf8 = true}) == "1");
    CHECK_FALSE(accepts("[1,2,]", options));
    CHECK_FALSE(accepts("// c\n1"));
    CHECK_FALSE(accepts("1 /* c */"));
  }
  SUBCASE("trailing commas") {
    const JsonParseOptions options{.allow_trailing_commas = true};
    CHECK(reparse("[1,2,]", options) == "[1,2]");
    CHECK(reparse("[1 , ]", options) == "[1]");
    CHECK(reparse(R"({"a":1,})", options) == R"({"a":1})");
    CHECK(reparse("[[1,],{\"a\":[2,],},]", options) == R"([[1],{"a":[2]}])");
    CHECK_FALSE(accepts("[,]", options));
    CHECK_FALSE(accepts("{,}", options));
    CHECK_FALSE(accepts("[1,,]", options));
    CHECK_FALSE(accepts("[,1]", options));
    CHECK_FALSE(accepts("[1,2,,]", options));
    CHECK_FALSE(accepts("{\"a\":1,,}", options));
    CHECK_FALSE(accepts("1,", options));
    CHECK_FALSE(accepts("[1,}", options));
    CHECK_FALSE(accepts("[1,", options));
    CHECK_FALSE(accepts("[1, // c\n]", options));
    CHECK(reparse("[1, // c\n]", JsonParseOptions{.allow_comments = true, .allow_trailing_commas = true}) == "[1]");
    CHECK_FALSE(accepts("[1,2,]"));
    CHECK_FALSE(accepts("{\"a\":1,}"));
  }
  SUBCASE("NaN and Infinity") {
    const JsonParseOptions options{.allow_nan_inf = true};
    const auto parsed = parse_json("[NaN, Infinity, -Infinity, 1]", options);
    REQUIRE(parsed.has_value());
    CHECK((*parsed)[0].is_float());
    CHECK(std::isnan((*parsed)[0].as_double()));
    CHECK((*parsed)[1].as_double() == infinity);
    CHECK((*parsed)[2].as_double() == -infinity);
    CHECK(dump_json(*parsed) == "[null,null,null,1]");
    CHECK(dump_json(*parsed, JsonWriteOptions{.nan_inf_as_literals = true}) == "[NaN,Infinity,-Infinity,1]");
    CHECK(reparse("{\"x\":NaN}", options) == R"({"x":null})");
    for (const std::string_view bad : {"nan", "NAN", "Nan", "inf", "Inf", "infinity", "INFINITY", "+Infinity", "-NaN", "+NaN", "-Inf",
                                       "Infinit", "-Infinit", "NaNx", "Infinityy", "- Infinity", "1e999", "-1e999"}) {
      CHECK_MESSAGE(!accepts(bad, options), "should reject: ", bad);
    }
    CHECK(reparse("-Infinit", options) == "error: invalid literal");
    CHECK(reparse("1e999", options) == "error: number out of range");  // an overflowing number is still an error
    CHECK_FALSE(accepts("NaN"));
    CHECK_FALSE(accepts("[Infinity]"));
    CHECK_FALSE(accepts("-Infinity"));
  }
  SUBCASE("single quotes") {
    const JsonParseOptions options{.allow_single_quotes = true};
    CHECK(reparse("'text'", options) == "\"text\"");
    CHECK(reparse("{'a':'b',\"c\":'d'}", options) == R"({"a":"b","c":"d"})");
    CHECK(reparse(R"('it\'s "quoted"')", options) == R"("it's \"quoted\"")");
    CHECK(reparse(R"('\n\u0041\ud83d\ude00\\')", options) == "\"\\nA\xF0\x9F\x98\x80\\\\\"");
    CHECK(reparse("\"it's\"", options) == "\"it's\"");
    CHECK(reparse("''", options) == "\"\"");
    CHECK(reparse("'abc", options) == "error: unterminated string");
    CHECK(reparse("'abc\"", options) == "error: unterminated string");
    CHECK(reparse(R"("a\'b")", options) == "error: invalid escape");  // \' only inside single quotes
    CHECK(reparse("'a\tb'", options) == "error: control character in string");
    CHECK_FALSE(accepts("'text'"));
    CHECK_FALSE(accepts("{'a':1}"));
    CHECK_FALSE(accepts(R"("a\'b")"));
  }
  SUBCASE("unquoted keys") {
    const JsonParseOptions options{.allow_unquoted_keys = true};
    CHECK(reparse("{a:1,_b:2,$c:3,d1:4,camelCase:5,\"quoted\":6}", options) == R"({"a":1,"_b":2,"$c":3,"d1":4,"camelCase":5,"quoted":6})");
    CHECK(reparse("{ key : 1 , other\n:\n2 }", options) == R"({"key":1,"other":2})");
    CHECK(reparse("{caf\xC3\xA9:1,\xE6\x97\xA5\xE6\x9C\xAC:2}", options) == "{\"caf\xC3\xA9\":1,\"\xE6\x97\xA5\xE6\x9C\xAC\":2}");
    CHECK(reparse("{true:1,null:2}", options) == R"({"true":1,"null":2})");  // keys, not literals
    CHECK(reparse("{a:{b:{c:[]}}}", options) == R"({"a":{"b":{"c":[]}}})");
    CHECK(reparse("{1a:1}", options) == "error: expected a string key");
    CHECK(reparse("{:1}", options) == "error: expected a string key");
    CHECK(reparse("{a b:1}", options) == "error: expected ':'");
    CHECK(reparse("{a-b:1}", options) == "error: expected ':'");
    CHECK(reparse("{a.b:1}", options) == "error: expected ':'");
    CHECK(reparse("{a:b}", options) == "error: unexpected character");  // values stay strict
    CHECK(reparse("{a", options) == "error: unexpected end of input");
    CHECK(reparse("{a\xFF:1}", options) == "error: invalid UTF-8");
    CHECK(reparse("{a\xFF:1}", JsonParseOptions{.allow_unquoted_keys = true, .replace_invalid_utf8 = true}) == "{\"a\xEF\xBF\xBD\":1}");
    CHECK_FALSE(accepts("{'a':1}", options));
    CHECK_FALSE(accepts("{a:1}"));
  }
  SUBCASE("control characters") {
    const JsonParseOptions options{.allow_control_characters = true};
    CHECK(parse_json("\"a\tb\nc\0d\x1F\""sv, options)->as_string() == "a\tb\nc\0d\x1F"sv);
    CHECK(reparse("\"a\tb\"", options) == R"("a\tb")");
    CHECK(reparse("{\"k\ney\":1}", options) == R"({"k\ney":1})");
    CHECK_FALSE(accepts("\"a\tb"sv, options));
    CHECK_FALSE(accepts("[1\0]"sv, options));  // still not whitespace
    CHECK_FALSE(accepts("\"a\tb\""));
  }
  SUBCASE("replacing invalid UTF-8") {
    const JsonParseOptions options{.replace_invalid_utf8 = true};
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK(parse_json("\"a\xFFz\"", options)->as_string() == "a" + fffd + "z");
    CHECK(parse_json("\"\xE2\x82\"", options)->as_string() == fffd);                    // truncated sequence: one replacement
    CHECK(parse_json("\"\xF0\x9F\x98\"", options)->as_string() == fffd);
    CHECK(parse_json("\"\xF0\x9F\x98x\"", options)->as_string() == fffd + "x");
    CHECK(parse_json("\"\xC0\x80\"", options)->as_string() == fffd + fffd);            // overlong: each byte
    CHECK(parse_json("\"\xED\xA0\x80\"", options)->as_string() == fffd + fffd + fffd);  // encoded surrogate
    CHECK(parse_json("\"\x80\xBF\"", options)->as_string() == fffd + fffd);
    CHECK(parse_json("\"ok \xC3\xA9 \xFF ok\"", options)->as_string() == "ok \xC3\xA9 " + fffd + " ok");
    CHECK(parse_json("{\"k\xFF\":1}", options)->items()[0].key() == "k" + fffd);
    // Lone surrogate escapes become U+FFFD as well.
    CHECK(parse_json(R"("\ud800")", options)->as_string() == fffd);
    CHECK(parse_json(R"("\udc00")", options)->as_string() == fffd);
    CHECK(parse_json(R"("\ud800x")", options)->as_string() == fffd + "x");
    CHECK(parse_json(R"("\ud83d\u0041")", options)->as_string() == fffd + "A");
    CHECK(parse_json(R"("\udc00\ud83d\ude00")", options)->as_string() == fffd + "\xF0\x9F\x98\x80");
    CHECK(parse_json(R"("\ud83d\ud83d\ude00")", options)->as_string() == fffd + "\xF0\x9F\x98\x80");
    // The result is always valid UTF-8.
    const auto repaired = parse_json("\"\x80\xC3\xE2\x82\xF0\x9F\xED\xA0\xFF\\udfff\\ud800\"", options);
    REQUIRE(repaired.has_value());
    CHECK(is_valid_utf8(repaired->as_string()));
    // Outside strings nothing is repaired, and other strictness stays.
    CHECK_FALSE(accepts("\xFF", options));
    CHECK_FALSE(accepts("[1,\xFF]", options));
    CHECK_FALSE(accepts(R"("\u12G4")", options));
    CHECK_FALSE(accepts("\"a\tb\"", options));
    CHECK_FALSE(accepts("\"a\xFFz\""));
  }
  SUBCASE("everything together") {
    const JsonParseOptions options{.allow_comments = true,
                                   .allow_trailing_commas = true,
                                   .allow_nan_inf = true,
                                   .allow_single_quotes = true,
                                   .allow_unquoted_keys = true,
                                   .allow_control_characters = true,
                                   .replace_invalid_utf8 = true};
    const std::string_view text = "// config\n{\n  name: 'demo', /* inline */\n  values: [1, 2.5, NaN, -Infinity,],\n  'tab\there': \"\xFF\",\n}\n";
    CHECK(dump_json(*parse_json(text, options), JsonWriteOptions{.nan_inf_as_literals = true}) ==
          "{\"name\":\"demo\",\"values\":[1,2.5,NaN,-Infinity],\"tab\\there\":\"\xEF\xBF\xBD\"}");
    // Strict JSON means the same thing in lenient mode.
    const std::string_view strict = R"({"a":[1,2.0,"x\n",true,null,{"b":-1e-3}]})";
    CHECK(reparse(strict, options) == reparse(strict));
    CHECK_FALSE(accepts(text));
  }
}

// ---------------------------------------------------------------------------
// Serialiser
// ---------------------------------------------------------------------------

TEST_CASE("json: compact and indented layouts are exact") {
  const Json document = json(R"({"z":1,"a":[true,null,"x"],"m":{"k":1.5,"deep":[[],{}]},"e":[],"o":{}})");
  CHECK(dump_json(document) == R"({"z":1,"a":[true,null,"x"],"m":{"k":1.5,"deep":[[],{}]},"e":[],"o":{}})");
  CHECK(dump_json(document, -1) == dump_json(document));
  CHECK(dump_json(document, -7) == dump_json(document));
  CHECK(dump_json(document, 2) ==
        "{\n"
        "  \"z\": 1,\n"
        "  \"a\": [\n"
        "    true,\n"
        "    null,\n"
        "    \"x\"\n"
        "  ],\n"
        "  \"m\": {\n"
        "    \"k\": 1.5,\n"
        "    \"deep\": [\n"
        "      [],\n"
        "      {}\n"
        "    ]\n"
        "  },\n"
        "  \"e\": [],\n"
        "  \"o\": {}\n"
        "}");
  CHECK(dump_json(json(R"({"a":[1,{"b":2}]})"), 4) ==
        "{\n"
        "    \"a\": [\n"
        "        1,\n"
        "        {\n"
        "            \"b\": 2\n"
        "        }\n"
        "    ]\n"
        "}");
  // Zero keeps the line structure without indentation.
  CHECK(dump_json(json(R"({"a":[1,{"b":2}]})"), 0) == "{\n\"a\": [\n1,\n{\n\"b\": 2\n}\n]\n}");
  CHECK(dump_json(json("[1,[2,3]]"), 1) == "[\n 1,\n [\n  2,\n  3\n ]\n]");
  // Scalars and empty containers have no line structure; no trailing newline anywhere.
  CHECK(dump_json(Json(), 2) == "null");
  CHECK(dump_json(Json(42), 2) == "42");
  CHECK(dump_json(Json("text"), 2) == "\"text\"");
  CHECK(dump_json(Json::array(), 2) == "[]");
  CHECK(dump_json(Json::object(), 2) == "{}");
  CHECK(dump_json(json("[[]]"), 2) == "[\n  []\n]");
  CHECK(dump_json(json(R"({"a":{}})"), 2) == "{\n  \"a\": {}\n}");
  CHECK(dump_json(Json(true)) == "true");
  CHECK(dump_json(Json(false)) == "false");

  // Both overloads agree.
  CHECK(dump_json(document, JsonWriteOptions{}) == dump_json(document));
  CHECK(dump_json(document, JsonWriteOptions{.indent = 2}) == dump_json(document, 2));
  CHECK(dump_json(document, JsonWriteOptions{.indent = 0}) == dump_json(document, 0));
}

TEST_CASE("json: writer options") {
  SUBCASE("ascii_only") {
    const JsonWriteOptions options{.ascii_only = true};
    const Json value = Json::object({{"caf\xC3\xA9", "\xE2\x82\xAC" "5 \xF0\x9F\x98\x80 \x7F ~ \xC2\x80"}});
    CHECK(dump_json(value, options) == R"({"caf\u00e9":"\u20ac5 \ud83d\ude00 \u007f ~ \u0080"})");
    CHECK(dump_json(Json("plain \"quoted\" \n"), options) == R"("plain \"quoted\" \n")");
    CHECK(dump_json(Json("\xEF\xBF\xBF\xF4\x8F\xBF\xBF"), options) == R"("\uffff\udbff\udfff")");
    const std::string text = dump_json(json(R"(["\u00e9\u4e2d\ud83d\ude00", {"\u00fc": 1}])"), options);
    bool ascii = true;
    for (const char c : text) ascii = ascii && static_cast<unsigned char>(c) < 0x7F;
    CHECK(ascii);
    CHECK((json(text) == json(R"(["\u00e9\u4e2d\ud83d\ude00", {"\u00fc": 1}])")));
    CHECK(dump_json(value) == "{\"caf\xC3\xA9\":\"\xE2\x82\xAC" "5 \xF0\x9F\x98\x80 \x7F ~ \xC2\x80\"}");  // default: UTF-8 as is
  }
  SUBCASE("escape_slash") {
    CHECK(dump_json(Json("</script> a/b"), JsonWriteOptions{.escape_slash = true}) == R"("<\/script> a\/b")");
    CHECK(dump_json(Json::object({{"a/b", 1}}), JsonWriteOptions{.escape_slash = true}) == R"({"a\/b":1})");
    CHECK(dump_json(Json("</script> a/b")) == R"("</script> a/b")");
    CHECK(reparse(R"("<\/script>")") == R"("</script>")");
  }
  SUBCASE("sort_keys") {
    const Json value = json(R"({"b":1,"a":{"z":[{"y":1,"x":2}],"y":2},"\u00e9":3,"B":4,"":5,"aa":6,"a b":7})");
    CHECK(dump_json(value, JsonWriteOptions{.sort_keys = true}) ==
          "{\"\":5,\"B\":4,\"a\":{\"y\":2,\"z\":[{\"x\":2,\"y\":1}]},\"a b\":7,\"aa\":6,\"b\":1,\"\xC3\xA9\":3}");
    // Sorting is a view: the value keeps its order, arrays are never reordered.
    CHECK(keys_of(value) == std::vector<std::string>{"b", "a", "\xC3\xA9", "B", "", "aa", "a b"});
    CHECK(dump_json(json("[3,1,2]"), JsonWriteOptions{.sort_keys = true}) == "[3,1,2]");
    CHECK(dump_json(json(R"({"b":1,"a":2})"), JsonWriteOptions{.indent = 1, .sort_keys = true}) == "{\n \"a\": 2,\n \"b\": 1\n}");
    // Byte order is code point order: U+E000 sorts before U+10000.
    CHECK(dump_json(json("{\"\xF0\x90\x80\x80\":1,\"\xEE\x80\x80\":2}"), JsonWriteOptions{.sort_keys = true}) ==
          "{\"\xEE\x80\x80\":2,\"\xF0\x90\x80\x80\":1}");
    Json large = Json::object();
    for (int i = 99; i >= 0; --i) large.set(std::string(1, static_cast<char>('a' + i % 26)) + std::to_string(i), i);
    const auto sorted = parse_json(dump_json(large, JsonWriteOptions{.sort_keys = true}));
    REQUIRE(sorted.has_value());
    bool ascending = true;
    for (std::size_t i = 1; i < sorted->items().size(); ++i) ascending = ascending && sorted->items()[i - 1].key() < sorted->items()[i].key();
    CHECK(ascending);
    CHECK((*sorted == large));
  }
  SUBCASE("newline") {
    CHECK(dump_json(json(R"({"a":[1]})"), JsonWriteOptions{.indent = 1, .newline = "\r\n"}) == "{\r\n \"a\": [\r\n  1\r\n ]\r\n}");
    CHECK(dump_json(json("[1,2]"), JsonWriteOptions{.indent = 2, .newline = ""}) == "[  1,  2]");
    CHECK(dump_json(json("[1,2]"), JsonWriteOptions{.newline = "\r\n"}) == "[1,2]");  // compact output has no lines
  }
  SUBCASE("nan_inf_as_literals") {
    const Json value = Json::array({nan_value, infinity, -infinity, 1.5});
    CHECK(dump_json(value) == "[null,null,null,1.5]");
    CHECK(dump_json(value, JsonWriteOptions{.nan_inf_as_literals = true}) == "[NaN,Infinity,-Infinity,1.5]");
    CHECK_FALSE(accepts("[NaN,Infinity,-Infinity,1.5]"));
    CHECK(accepts("[NaN,Infinity,-Infinity,1.5]", JsonParseOptions{.allow_nan_inf = true}));
  }
  SUBCASE("combined") {
    const Json value = json(R"({"b":"\u00e9/","a":[1.0,{"d":null,"c":"\ud83d\ude00"}]})");
    const JsonWriteOptions options{.indent = 2, .ascii_only = true, .escape_slash = true, .sort_keys = true, .newline = "\r\n"};
    CHECK(dump_json(value, options) ==
          "{\r\n  \"a\": [\r\n    1.0,\r\n    {\r\n      \"c\": \"\\ud83d\\ude00\",\r\n      \"d\": null\r\n    }\r\n  ],\r\n  \"b\": \"\\u00e9\\/\"\r\n}");
    CHECK((json(dump_json(value, options)) == value));
  }
}

TEST_CASE("json: documents round-trip through text unchanged") {
  for (const std::string_view text : {
           R"(null)",
           R"([])",
           R"({})",
           R"([0,-1,1.5,-2.5e-10,1e+100,18446744073709551615,-9223372036854775808,0.1,100.0])",
           R"({"z":1,"a":{"y":[1,{"x":null}],"b":"text"},"":"","unicode":"caf\u00e9","escaped":"\"\\\n\t"})",
           R"([[[[[[[[[[1]]]]]]]]],[{"a":[{"b":[{"c":{}}]}]}],"",[""],{"":{"":{"":[]}}}])",
           R"({"numbers":[1,1.0,1e0,10,10.0,1e1],"same key twice?":"no"})",
       }) {
    const auto parsed = parse_json(text);
    CHECK_MESSAGE(parsed.has_value(), "should parse: ", text);
    if (!parsed) continue;
    const std::string compact = dump_json(*parsed);
    const auto again = parse_json(compact);
    REQUIRE(again.has_value());
    CHECK_MESSAGE(again->equal_ordered(*parsed), "compact round trip of ", text);
    CHECK(dump_json(*again) == compact);
    for (const int indent : {0, 1, 2, 8}) {
      const auto pretty = parse_json(dump_json(*parsed, indent));
      REQUIRE(pretty.has_value());
      CHECK_MESSAGE(pretty->equal_ordered(*parsed), "indent ", indent, " round trip of ", text);
    }
    const auto escaped = parse_json(dump_json(*parsed, JsonWriteOptions{.indent = 3, .ascii_only = true, .escape_slash = true, .newline = "\r\n"}));
    REQUIRE(escaped.has_value());
    CHECK(escaped->equal_ordered(*parsed));
  }
  // Number kinds survive: integers stay integers and doubles stay doubles.
  const auto kinds = parse_json(dump_json(*parse_json("[1,1.0,1e0,18446744073709551615,-1,1e300]")));
  REQUIRE(kinds.has_value());
  CHECK((*kinds)[0].type() == Json::Type::Int64);
  CHECK((*kinds)[1].type() == Json::Type::Double);
  CHECK((*kinds)[2].type() == Json::Type::Double);
  CHECK((*kinds)[3].type() == Json::Type::UInt64);
  CHECK((*kinds)[4].type() == Json::Type::Int64);
  CHECK((*kinds)[5].type() == Json::Type::Double);
}

// ---------------------------------------------------------------------------
// Hostile shapes and sizes
// ---------------------------------------------------------------------------

TEST_CASE("json: very deep values are safe to build, copy, compare, write and destroy") {
  constexpr std::size_t depth = 1'000'000;
  Json deep = Json::array();
  for (std::size_t i = 1; i < depth; ++i) {
    Json outer = Json::array();
    outer.push_back(std::move(deep));
    deep = std::move(outer);
  }

  // Serialisation is iterative.
  const std::string text = dump_json(deep);
  CHECK(text.size() == 2 * depth);
  CHECK(text == std::string(depth, '[') + std::string(depth, ']'));

  // So are copying and comparing.
  Json copy = deep;
  CHECK((copy == deep));
  CHECK(copy.equal_ordered(deep));
  Json* innermost = &copy;
  std::size_t levels = 1;
  while (Json* next = innermost->at(0)) {
    innermost = next;
    ++levels;
  }
  CHECK(levels == depth);
  innermost->push_back(1);
  CHECK_FALSE((copy == deep));
  CHECK(dump_json(copy).size() == 2 * depth + 1);

  // Parsing it back needs a matching depth limit, and nothing else.
  CHECK_FALSE(parse_json(text));
  auto reparsed = parse_json(text, depth);
  REQUIRE(reparsed.has_value());
  CHECK((*reparsed == deep));

  // Moving is O(1) and destruction is iterative.
  Json moved = std::move(deep);
  CHECK(deep.is_null());  // NOLINT(bugprone-use-after-move)
  moved = nullptr;
  copy = Json();
  reparsed = Json();

  // The indented form costs depth * indent characters per line, so something
  // very deep is only printable with zero indent, where it is just lines.
  constexpr std::size_t tall = 50'000;
  const std::string tower = std::string(tall, '[') + std::string(tall, ']');
  auto tall_value = parse_json(tower, tall);
  REQUIRE(tall_value.has_value());
  CHECK_FALSE(parse_json(tower, tall - 1));
  const std::string lines = dump_json(*tall_value, 0);
  CHECK(lines.size() == 4 * tall - 2);
  auto from_lines = parse_json(lines, tall);
  REQUIRE(from_lines.has_value());
  CHECK((*from_lines == *tall_value));

  // The same with objects, and with wide levels along the way.
  Json chain = "leaf";
  constexpr std::size_t links = 50'000;
  for (std::size_t i = 0; i < links; ++i) {
    Json outer = Json::object();
    if (i % 1000 == 0) {
      for (int k = 0; k < 40; ++k) outer.set("pad" + std::to_string(k), Json::array({k, "x"}));
    }
    outer.set("next", std::move(chain));
    chain = std::move(outer);
  }
  const Json chain_copy = chain;
  CHECK((chain_copy == chain));
  const std::string chain_text = dump_json(chain);
  auto chain_back = parse_json(chain_text, links + 1);
  REQUIRE(chain_back.has_value());
  CHECK(chain_back->equal_ordered(chain));
  CHECK(dump_json(chain, JsonWriteOptions{.sort_keys = true}).size() == chain_text.size());
}

TEST_CASE("json: large inputs are handled in linear time") {
  constexpr std::size_t megabyte = 1024 * 1024;

  SUBCASE("one long string") {
    const std::string body = repeat("The quick brown fox jumps over the lazy dog. ", 10 * megabyte / 45);
    const std::string text = "\"" + body + "\"";
    const auto parsed = parse_json(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->as_string() == body);
    CHECK(dump_json(*parsed) == text);
    CHECK(reparse(text.substr(0, text.size() - 1)) == "error: unterminated string");
  }
  SUBCASE("one long string of escapes and multi-byte characters") {
    const std::string unit = R"(\n\"\\\u00e9\ud83d\ude00)" "\xE6\x97\xA5\xF0\x9F\x98\x80";
    const std::string text = "\"" + repeat(unit, 4 * megabyte / unit.size()) + "\"";
    const auto parsed = parse_json(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->as_string().size() == (4 * megabyte / unit.size()) * 16);
    const auto again = parse_json(dump_json(*parsed, JsonWriteOptions{.ascii_only = true}));
    REQUIRE(again.has_value());
    CHECK(again->as_string() == parsed->as_string());
  }
  SUBCASE("one object with many small members") {
    std::string text = "{";
    std::size_t count = 0;
    while (text.size() < 4 * megabyte) {
      text += "\"k" + std::to_string(count) + "\":" + std::to_string(count % 100) + ",";
      ++count;
    }
    text.back() = '}';
    const auto parsed = parse_json(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->size() == count);
    CHECK((*parsed)["k" + std::to_string(count - 1)].as_int64(-1) == static_cast<std::int64_t>((count - 1) % 100));
    CHECK(parsed->items()[count / 2].key() == "k" + std::to_string(count / 2));
    CHECK(dump_json(*parsed) == text);
    // The same member repeated is as cheap as distinct ones.
    const std::string repeated = "{" + repeat("\"same\":1,", 4 * megabyte / 9) + "\"same\":2}";
    const auto collapsed = parse_json(repeated);
    REQUIRE(collapsed.has_value());
    CHECK(dump_json(*collapsed) == R"({"same":2})");
  }
  SUBCASE("many small objects") {
    std::string text = "[";
    std::size_t count = 0;
    while (text.size() < 4 * megabyte) {
      text += R"({"id":)" + std::to_string(count) + R"(,"tags":["a","b"],"ok":true,"v":null,"p":{"x":1.5,"y":-2}},)";
      ++count;
    }
    text.back() = ']';
    const auto parsed = parse_json(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->size() == count);
    CHECK((*parsed)[count - 1]["id"].as_uint64() == count - 1);
    CHECK(dump_json(*parsed) == text);
    const Json copy = *parsed;
    CHECK(copy.equal_ordered(*parsed));
  }
  SUBCASE("long arrays of numbers") {
    std::string text = "[";
    std::size_t count = 0;
    while (text.size() < 4 * megabyte) {
      text += count % 3 == 0 ? std::to_string(count) + "," : count % 3 == 1 ? "-" + std::to_string(count) + ".5," : "1e+30,";
      ++count;
    }
    text.back() = ']';
    const auto parsed = parse_json(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->size() == count);
    CHECK(dump_json(*parsed) == text);
  }
  SUBCASE("hostile input fails fast") {
    CHECK(reparse(std::string(10 * megabyte, '[')).starts_with("error: nesting too deep"));
    CHECK(reparse(repeat("{\"a\":", 2 * megabyte)).starts_with("error: nesting too deep"));
    CHECK(reparse(std::string(megabyte, '9')) == "error: number out of range");
    CHECK(reparse("0." + std::string(megabyte, '9')) == "1.0");
    CHECK(reparse("1e" + std::string(megabyte, '9')) == "error: number out of range");
    CHECK(reparse("1e-" + std::string(megabyte, '9')) == "0.0");
    CHECK(reparse(std::string(10 * megabyte, ' ')) == "error: unexpected end of input");
    CHECK(reparse("\"" + std::string(10 * megabyte, '\\')) == "error: unterminated string");
    CHECK(reparse(repeat("/* ", 3 * megabyte), JsonParseOptions{.allow_comments = true}) == "error: unterminated comment");
    JsonLocation where;
    const std::string late = repeat("[1,2,3],\n", megabyte / 4) + "!";
    CHECK_FALSE(parse_json("[" + late, JsonParseOptions{}, &where));
    CHECK(where.line == megabyte / 4 + 1);
    CHECK(where.column == 1);
    CHECK(where.offset == late.size());
  }
}
