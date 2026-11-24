// Encode -> decode round trips: every format alone and every pair of formats
// through the IR, with exact equality for everything the formats can hold.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string_view>

#include "families/data/data_test_support.hpp"

using namespace cvt;
namespace td = cvt::test::data;

namespace {

// Every value type the general-purpose formats share: Unicode beyond the BMP,
// escapes, 64-bit extremes, shortest-round-trip doubles, empty containers and
// deep nesting.
constexpr std::string_view universal = R"({
  "string": "hello", "unicode": "héllo ✓ 日本語 😀", "escapes": "q\" b\\ t\t n\n r\r",
  "empty_string": "", "space": " padded ",
  "int": 42, "negative": -17, "zero": 0, "max": 9223372036854775807, "min": -9223372036854775808,
  "float": 3.141592653589793, "neg_float": -0.5, "exp": 6.02214076e23, "tiny": 5e-324, "whole_float": 2.0,
  "small_float": 1e-7, "big_float": 1.7976931348623157e308,
  "true": true, "false": false, "null": null,
  "empty_list": [], "empty_object": {},
  "list": [1, "two", 3.25, null, true, [], {}],
  "nested": {"a": {"b": {"c": [1, [2, [3, {"d": "deep"}]]]}}},
  "records": [{"id": 1, "name": "alpha"}, {"id": 2, "name": "beta"}],
  "key with spaces": 1, "": "empty key", "123": "numeric key", "yes": "no", "true_string": "true", "null_string": "null"
})";

constexpr std::string_view universal_formats[] = {"json",  "jsonc",  "json5", "hjson", "yaml",  "cbor",
                                                  "msgpack", "bson", "ubjson", "bjdata", "smile", "ion",
                                                  "edn",   "python-literal", "kdl"};

}  // namespace

TEST_CASE("data/roundtrip: each general-purpose format holds the universal data exactly") {
  const Json value = td::json(universal);
  for (const std::string_view format : universal_formats) {
    INFO("format ", format);
    const auto back = td::round_trip(format, value);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::data_equal(*back, value), test::data_diff(*back, value));
  }
}

TEST_CASE("data/roundtrip: every pair of general-purpose formats through the IR") {
  const Json value = td::json(universal);
  for (const std::string_view a : universal_formats) {
    const auto first = td::round_trip(a, value);
    REQUIRE_OK(first);
    for (const std::string_view b : universal_formats) {
      INFO(a, " -> ", b);
      const auto second = td::round_trip(b, *first);
      REQUIRE_OK(second);
      CHECK_MESSAGE(test::data_equal(*second, value), test::data_diff(*second, value));
    }
  }
}

TEST_CASE("data/roundtrip: the same-format compress path re-encodes without loss") {
  const Json value = td::json(universal);
  for (const std::string_view format : universal_formats) {
    INFO("format ", format);
    const auto encoded = td::write_bytes(format, value);
    REQUIRE_OK(encoded);
    const test::Converted again = test::convert(*encoded, format, format);
    REQUIRE(again.ok());
    const auto back = td::read_bytes(format, again.data);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::data_equal(*back, value), test::data_diff(*back, value));
  }
}

TEST_CASE("data/roundtrip: compressing JSON minifies it and is smaller") {
  const Json value = td::json(universal);
  const auto pretty = td::write("json", value);
  REQUIRE_OK(pretty);
  const test::Converted small = test::convert(*pretty, "json", "json");
  REQUIRE(small.ok());
  CHECK(small.data.size() < pretty->size());
  CHECK(small.text().find('\n') == std::string::npos);
  // Asking for indentation keeps the pretty form.
  const test::Converted kept = test::convert(*pretty, "json", "json", R"({"indent": 2})");
  REQUIRE(kept.ok());
  CHECK(kept.text() == *pretty);
}

TEST_CASE("data/roundtrip: values without a JSON spelling survive where the format has them") {
  // Binary data, dates, integers beyond 64 bits, exact decimals and specials.
  const Json value = td::json(R"({
    "binary": {"$binary": "AAEC/f7/"}, "date": {"$date": "2024-05-01T12:30:45.123456Z"},
    "bigint": {"$bigint": "123456789012345678901234567890"}, "negbig": {"$bigint": "-98765432109876543210"},
    "decimal": {"$decimal": "3.14159265358979323846264338327950288"},
    "nan": {"$float": "NaN"}, "inf": {"$float": "Infinity"}, "neginf": {"$float": "-Infinity"},
    "uint64": 18446744073709551615
  })");
  struct Case {
    std::string_view format;
    std::vector<std::string_view> keys;  // the members that must come back exactly
  };
  const Case cases[] = {
      {"json", {"binary", "date", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"yaml", {"binary", "date", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"cbor", {"binary", "date", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"ion", {"binary", "date", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"smile", {"binary", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"msgpack", {"binary", "date", "nan", "inf", "neginf", "uint64"}},
      {"bson", {"binary", "nan", "inf", "neginf"}},
      {"toml", {"date", "nan", "inf", "neginf", "decimal"}},
      {"edn", {"date", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"ubjson", {"binary", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"bjdata", {"binary", "bigint", "negbig", "decimal", "nan", "inf", "neginf", "uint64"}},
      {"bplist", {"binary", "nan", "inf", "neginf", "uint64"}},
      {"plist", {"binary", "nan", "inf", "neginf", "uint64"}},
      {"python-literal", {"bigint", "negbig", "inf", "neginf", "uint64"}},
  };
  for (const Case& c : cases) {
    INFO("format ", c.format);
    const auto back = td::round_trip(c.format, value);
    REQUIRE_OK(back);
    for (const std::string_view key : c.keys) {
      INFO("member ", key);
      CHECK_MESSAGE(test::data_equal((*back)[key], value[key]), test::data_diff((*back)[key], value[key]));
    }
  }
  // BSON keeps big integers and decimals of up to 34 digits as decimal128.
  const Json bson = td::round_trip("bson", td::json(R"({"i": {"$bigint": "-98765432109876543210"},
      "d": {"$decimal": "1234567890.123456789012345678901234"}})")).value();
  CHECK(bson == td::json(R"({"i": {"$decimal": "-98765432109876543210"}, "d": {"$decimal": "1234567890.123456789012345678901234"}})"));
  // BSON dates hold milliseconds, plists whole seconds.
  CHECK(td::round_trip("bson", value).value()["date"] == td::json(R"({"$date": "2024-05-01T12:30:45.123Z"})"));
  CHECK(td::round_trip("bplist", value).value()["date"] == td::json(R"({"$date": "2024-05-01T12:30:45.123456Z"})"));
  CHECK(td::round_trip("plist", value).value()["date"] == td::json(R"({"$date": "2024-05-01T12:30:45Z"})"));
}

TEST_CASE("data/roundtrip: binary formats keep their own tagged extras") {
  // MessagePack extension types, BSON-only types, CBOR tags and plist UIDs.
  const Json msgpack = td::json(R"({"ext": {"$ext": 7, "$binary": "AQID"}})");
  CHECK(td::round_trip("msgpack", msgpack).value() == msgpack);
  const Json bson = td::json(R"({"oid": {"$oid": "65f1a2b3c4d5e6f708192a3b"}, "re": {"$regex": "^a", "$options": "i"},
      "ts": {"$timestamp": {"t": 1700000000, "i": 7}}, "min": {"$minKey": 1}, "max": {"$maxKey": 1},
      "code": {"$code": "x = 1"}, "sym": {"$symbol": "s"}, "undef": {"$undefined": true},
      "uuid": {"$binary": "AAECAwQFBgcICQoLDA0ODw==", "$subtype": 4}})");
  CHECK_MESSAGE(test::data_equal(td::round_trip("bson", bson).value(), bson), test::data_diff(td::round_trip("bson", bson).value(), bson));
  const Json cbor = td::json(R"({"tag": {"$tag": 32, "$value": "http://example.invalid/"}, "undef": {"$undefined": true},
      "day": {"$date": "2024-05-01"}})");
  CHECK(td::round_trip("cbor", cbor).value() == cbor);
  const Json plist = td::json(R"({"uid": {"$uid": 42}})");
  CHECK(td::round_trip("bplist", plist).value() == plist);
  CHECK(td::round_trip("plist", plist).value() == plist);
}

TEST_CASE("data/roundtrip: text formats with fewer types keep their values as text") {
  const Json value = td::json(R"({"section": {"name": "demo", "port": "8080", "path": "/a b;c", "quote": "\"q\""},
                                  "other": {"empty": "", "unicode": "é✓"}})");
  for (const std::string_view format : {"ini", "toml", "yaml", "plist", "properties"}) {
    INFO("format ", format);
    const auto back = td::round_trip(format, value, {}, format == "properties" ? R"({"nested_keys": true})" : "");
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::data_equal(*back, value), test::data_diff(*back, value));
  }
}

TEST_CASE("data/roundtrip: script and record formats") {
  const Json value = td::json(R"({"name": "alpha", "count": 3, "ratio": 0.5, "ok": true, "list": ["x", "y"],
                                  "nested": {"a": {"b": [1, 2]}}, "unicode": "é😀"})");
  for (const std::string_view format : {"lua-table", "php-serialized", "python-literal", "edn", "hjson", "kdl", "sexp"}) {
    INFO("format ", format);
    const auto back = td::round_trip(format, value);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::data_equal(*back, value), test::data_diff(*back, value));
  }
  // Avro: a list of records with an inferred schema.
  const Json records = td::json(R"([{"id": 1, "name": "a", "score": 1.5, "tags": ["x"], "when": {"$date": "2024-05-01T12:30:45.5Z"}},
                                    {"id": 2, "name": "b", "score": null, "tags": [], "when": {"$date": "1969-12-31T23:59:59Z"}, "extra": true}])");
  const auto avro = td::round_trip("avro", records);
  REQUIRE_OK(avro);
  const Json expected = td::json(R"([{"id": 1, "name": "a", "score": 1.5, "tags": ["x"], "when": {"$date": "2024-05-01T12:30:45.5Z"}, "extra": null},
                                     {"id": 2, "name": "b", "score": null, "tags": [], "when": {"$date": "1969-12-31T23:59:59Z"}, "extra": true}])");
  CHECK_MESSAGE(test::data_equal(*avro, expected), test::data_diff(*avro, expected));
  // Query strings and bencode hold strings and integers.
  const Json strings = td::json(R"({"a": "1", "list": ["x", "y z"], "nested": {"k": "é&="}})");
  CHECK(td::round_trip("querystring", strings).value() == strings);
  const Json torrent = td::json(R"({"announce": "http://tracker.example.invalid/", "info": {"length": 10, "pieces": {"$binary": "AP8Q"}}})");
  CHECK(td::round_trip("bencode", torrent).value() == torrent);
}

TEST_CASE("data/roundtrip: deep nesting up to the limit") {
  std::string deep;
  for (int i = 0; i < 200; ++i) deep += "[";
  deep += "1";
  for (int i = 0; i < 200; ++i) deep += "]";
  const Json value = td::json(deep);
  for (const std::string_view format : universal_formats) {
    if (format == "kdl" || format == "bson") continue;  // nested arrays become nodes / documents
    INFO("format ", format);
    const auto back = td::round_trip(format, value);
    REQUIRE_OK(back);
    CHECK(test::data_equal(*back, value));
  }
}
