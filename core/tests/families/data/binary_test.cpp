// Binary formats: specification vectors, encoder choices and hostile input.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/data/data_test_support.hpp"
#include "util/hex.hpp"

using namespace cvt;
using namespace std::string_view_literals;
namespace td = cvt::test::data;

namespace {

Bytes hex(std::string_view text) { return util::hex_decode(text).value_or(Bytes{}); }

std::string hex_of(const Bytes& bytes) { return util::hex_encode(bytes); }

Json read_hex(std::string_view format, std::string_view text, std::string_view options = {}) {
  const auto value = td::read_bytes(format, hex(text), options);
  if (!value) {
    FAIL_CHECK(format, " ", text, ": ", value.error().message);
    return Json();
  }
  return *value;
}

Bytes repeat(std::string_view chunk, std::size_t times) {
  Bytes out;
  for (std::size_t i = 0; i < times; ++i) append(out, chunk);
  return out;
}

}  // namespace

TEST_CASE("data/msgpack: every type byte decodes") {
  CHECK(read_hex("msgpack", "05") == Json(5));
  CHECK(read_hex("msgpack", "ff") == Json(-1));
  CHECK(read_hex("msgpack", "c0").is_null());
  CHECK(read_hex("msgpack", "c3") == Json(true));
  CHECK(read_hex("msgpack", "ca3fc00000") == Json(1.5));
  CHECK(read_hex("msgpack", "cb3ff8000000000000") == Json(1.5));
  CHECK(read_hex("msgpack", "cfffffffffffffffff") == Json(std::uint64_t{18446744073709551615u}));
  CHECK(read_hex("msgpack", "d3ffffffffffffffff") == Json(-1));
  CHECK(read_hex("msgpack", "d90161") == Json("a"));
  CHECK(read_hex("msgpack", "c5000201ff") == td::json(R"({"$binary": "Af8="})"));
  CHECK(read_hex("msgpack", "de0002a1610102c3") == td::json(R"({"a": 1, "2": true})"));
  CHECK(read_hex("msgpack", "dc000201a162") == td::json(R"([1, "b"])"));
  CHECK(read_hex("msgpack", "d6ff00000000") == td::json(R"({"$date": "1970-01-01T00:00:00Z"})"));
  CHECK(read_hex("msgpack", "d7ff0000000400000000") == td::json(R"({"$date": "1970-01-01T00:00:00.000000001Z"})"));
  CHECK(read_hex("msgpack", "c70cff00000000ffffffffffffffff") == td::json(R"({"$date": "1969-12-31T23:59:59Z"})"));
  CHECK(read_hex("msgpack", "d40501") == td::json(R"({"$ext": 5, "$binary": "AQ=="})"));
  CHECK(read_hex("msgpack", "0102") == td::json("[1, 2]"));  // a stream of two values
}

TEST_CASE("data/msgpack: the writer picks the smallest encoding") {
  const auto bytes = td::write_bytes("msgpack", td::json(R"([127, 128, -32, -33, 65536, 1.5, 1.1, "a", true, null, {"$binary": "AQ=="}])"));
  REQUIRE_OK(bytes);
  CHECK(hex_of(*bytes) == "9b7fcc80e0d0dfce00010000ca3fc00000cb3ff199999999999aa161c3c0c40101");
  const auto doubles = td::write_bytes("msgpack", td::json("[1.5]"), R"({"float_size": "64"})");
  REQUIRE_OK(doubles);
  CHECK(hex_of(*doubles) == "91cb3ff8000000000000");
  CHECK(hex_of(td::write_bytes("msgpack", td::json(R"({"$date": "1970-01-01T00:00:01Z"})")).value()) == "d6ff00000001");
}

TEST_CASE("data/msgpack: hostile input") {
  CHECK_STATUS(td::read_bytes("msgpack", hex("ddffffffff")), Status::Decode);  // 4 billion elements promised
  CHECK_STATUS(td::read_bytes("msgpack", hex("c6ffffffff00")), Status::Decode);
  CHECK_STATUS(td::read_bytes("msgpack", hex("c1")), Status::Decode);
  CHECK_STATUS(td::read_bytes("msgpack", repeat("\x91", 5000)), Status::Limit);
  CHECK_STATUS(td::read_bytes("msgpack", hex("d6ff")), Status::Decode);
  CHECK_STATUS(td::read_bytes("msgpack", {}), Status::Decode);
}

TEST_CASE("data/cbor: RFC 8949 Appendix A examples") {
  CHECK(read_hex("cbor", "1903e8") == Json(1000));
  CHECK(read_hex("cbor", "3903e7") == Json(-1000));
  CHECK(read_hex("cbor", "1bffffffffffffffff") == Json(std::uint64_t{18446744073709551615u}));
  CHECK(read_hex("cbor", "c249010000000000000000") == td::json(R"({"$bigint": "18446744073709551616"})"));
  CHECK(read_hex("cbor", "3bffffffffffffffff") == td::json(R"({"$bigint": "-18446744073709551616"})"));
  CHECK(read_hex("cbor", "c349010000000000000000") == td::json(R"({"$bigint": "-18446744073709551617"})"));
  CHECK(read_hex("cbor", "f90001").as_double() == 5.960464477539063e-8);
  CHECK(read_hex("cbor", "f97bff").as_double() == 65504.0);
  CHECK(read_hex("cbor", "fa47c35000").as_double() == 100000.0);
  CHECK(std::isinf(read_hex("cbor", "f97c00").as_double()));
  CHECK(std::isnan(read_hex("cbor", "f97e00").as_double()));
  CHECK(read_hex("cbor", "c074323031332d30332d32315432303a30343a30305a") == td::json(R"({"$date": "2013-03-21T20:04:00Z"})"));
  CHECK(read_hex("cbor", "c11a514b67b0") == td::json(R"({"$date": "2013-03-21T20:04:00Z"})"));
  CHECK(read_hex("cbor", "c1fb41d452d9ec200000") == td::json(R"({"$date": "2013-03-21T20:04:00.5Z"})"));
  CHECK(read_hex("cbor", "d82076687474703a2f2f7777772e6578616d706c652e636f6d") ==
        td::json(R"({"$tag": 32, "$value": "http://www.example.com"})"));
  CHECK(read_hex("cbor", "c48221196ab3") == Json(273.15));
  CHECK(read_hex("cbor", "f7") == td::json(R"({"$undefined": true})"));
  CHECK(read_hex("cbor", "a201020304") == td::json(R"({"1": 2, "3": 4})"));
  CHECK(read_hex("cbor", "d9d9f78101") == td::json("[1]"));
  CHECK(read_hex("cbor", "5f42010243030405ff") == td::json(R"({"$binary": "AQIDBAU="})"));
}

TEST_CASE("data/cbor: preferred serialisation") {
  CHECK(hex_of(td::write_bytes("cbor", td::json("[1.0, 100000.0, 1.1, 0.0, -0.0, 65504.0, 5.960464477539063e-8]")).value()) ==
        "87f93c00fa47c35000fb3ff199999999999af90000f98000f97bfff90001");
  CHECK(hex_of(td::write_bytes("cbor", td::json("[23, 24, 255, 256, -24, -25, 4294967296]")).value()) ==
        "87171818" "18ff" "190100" "37" "3818" "1b0000000100000000");
  CHECK(hex_of(td::write_bytes("cbor", td::json(R"({"$bigint": "18446744073709551616"})")).value()) == "c249010000000000000000");
  CHECK(hex_of(td::write_bytes("cbor", td::json(R"({"$bigint": "-18446744073709551617"})")).value()) == "c349010000000000000000");
  CHECK(hex_of(td::write_bytes("cbor", td::json(R"({"$decimal": "273.15"})")).value()) == "c48221196ab3");
  CHECK(hex_of(td::write_bytes("cbor", td::json(R"({"$date": "2013-03-21T20:04:00Z"})"), R"({"cbor_dates": "epoch"})").value()) ==
        "c11a514b67b0");
  CHECK(hex_of(td::write_bytes("cbor", td::json("1"), R"({"self_describe": true})").value()) == "d9d9f701");
  CHECK(hex_of(td::write_bytes("cbor", td::json("[1.5]"), R"({"float_size": "64"})").value()) == "81fb3ff8000000000000");
}

TEST_CASE("data/cbor: hostile input") {
  CHECK_STATUS(td::read_bytes("cbor", hex("9bffffffffffffffff")), Status::Decode);
  CHECK_STATUS(td::read_bytes("cbor", hex("5bffffffffffffffff00")), Status::Decode);
  CHECK_STATUS(td::read_bytes("cbor", repeat("\x81", 5000)), Status::Limit);
  CHECK_STATUS(td::read_bytes("cbor", hex("9f01")), Status::Decode);   // never closed
  CHECK_STATUS(td::read_bytes("cbor", hex("ff")), Status::Decode);     // stray break
  CHECK_STATUS(td::read_bytes("cbor", hex("1c")), Status::Decode);     // reserved
  CHECK_STATUS(td::read_bytes("cbor", hex("5f01ff")), Status::Decode); // bad chunk
  CHECK_STATUS(td::read_bytes("cbor", repeat("\x9f", 3000)), Status::Limit);
}

TEST_CASE("data/bson: element types and decimal128") {
  // {"d": Decimal128("0.1")}
  const Bytes decimal = hex("1800000013640001000000000000000000000000003e3000");
  CHECK(td::read_bytes("bson", decimal).value() == td::json(R"({"d": {"$decimal": "0.1"}})"));
  CHECK(td::write_bytes("bson", td::json(R"({"d": {"$decimal": "0.1"}})")).value() == decimal);
  CHECK(td::write_bytes("bson", td::json(R"({"d": {"$decimal": "1e-1"}})")).value() == decimal);
  // int32 / int64 / double / bool / null / string choices.
  const auto bytes = td::write_bytes("bson", td::json(R"({"a": 1, "b": 5000000000, "c": 1.5, "d": true, "e": null, "f": "x"})"));
  REQUIRE_OK(bytes);
  CHECK(hex_of(*bytes) == "320000001061000100000012620000f2052a01000000016300000000000000f83f"
                          "086400010a650002660002000000780000");
  CHECK(td::read_bytes("bson", *bytes).value() == td::json(R"({"a": 1, "b": 5000000000, "c": 1.5, "d": true, "e": null, "f": "x"})"));
  // Non-object roots are wrapped, lists of objects become consecutive documents.
  std::vector<std::string> warnings;
  CHECK(td::round_trip("bson", td::json("[1, 2]")).value() == td::json(R"({"value": [1, 2]})"));
  CHECK(td::round_trip("bson", td::json(R"([{"a": 1}, {"b": 2}])")).value() == td::json(R"([{"a": 1}, {"b": 2}])"));
}

TEST_CASE("data/bson: hostile input") {
  CHECK_STATUS(td::read_bytes("bson", hex("ffffff7f00")), Status::Decode);          // size beyond the data
  CHECK_STATUS(td::read_bytes("bson", hex("0c000000036100ffffff7f0000")), Status::Decode);
  CHECK_STATUS(td::read_bytes("bson", hex("0d00000002610005000000610000")), Status::Decode);
  CHECK_STATUS(td::read_bytes("bson", hex("0800000099610000")), Status::Decode);    // unknown type
  CHECK_STATUS(td::write_bytes("bson", td::json(R"({"a\u0000b": 1})")), Status::Encode);
}

TEST_CASE("data/ubjson and bjdata: containers") {
  CHECK(td::read("ubjson", "[$i#i\x03\x01\x02\x03"sv).value() == td::json("[1, 2, 3]"));
  CHECK(td::read("ubjson", "{i\x01" "aSi\x01" "bi\x01" "cZ}"sv).value() == td::json(R"({"a": "b", "c": null})"));
  CHECK(td::read("ubjson", "[NTNF]"sv).value() == td::json("[true, false]"));
  CHECK(td::read("ubjson", "[$U#i\x02\x01\xff"sv).value() == td::json(R"({"$binary": "Af8="})"));
  CHECK(td::read("ubjson", "[$U#i\x02\x01\xff"sv, R"({"uint8_arrays_as_binary": false})").value() == td::json("[1, 255]"));
  CHECK(td::read("ubjson", "Hi\x1e" "123456789012345678901234567890"sv).value() == td::json("123456789012345678901234567890"));
  CHECK(td::read("bjdata", "[$U#[$U#U\x02\x02\x03\x01\x02\x03\x04\x05\x06"sv).value() == td::json("[[1, 2, 3], [4, 5, 6]]"));
  CHECK(td::read("bjdata", "M\xff\xff\xff\xff\xff\xff\xff\xff"sv).value() == Json(std::uint64_t{18446744073709551615u}));
  CHECK(td::read("bjdata", "h\x00\x3c"sv).value() == Json(1.0));
  CHECK(hex_of(td::write_bytes("ubjson", td::json(R"([1, 300, 1.5, "a", null])")).value()) == "5b690149012c64" "3fc00000" "536901615a" "5d");
  CHECK(hex_of(td::write_bytes("bjdata", td::json("[300]")).value()) == "5b492c015d");
  CHECK_STATUS(td::read("ubjson", "[$Z#L\x7f\xff\xff\xff\xff\xff\xff\xff"sv), Status::Limit);
  CHECK_STATUS(td::read("ubjson", "[$i#L\x00\x00\x00\x00\x00\x00\x10\x00"sv), Status::Decode);
  CHECK_STATUS(td::read("ubjson", "Si\xff"sv), Status::Decode);
  CHECK_STATUS(td::read_bytes("ubjson", repeat("[", 5000)), Status::Limit);
}

TEST_CASE("data/smile: tokens and hostile input") {
  // ":)\n" header, {"a": 1, "b": [true, "xy"]}
  CHECK(td::read("smile", ":)\n\x00\xfa\x80" "a\xc2\x80" "b\xf8\x23\x41xy\xf9\xfb"sv).value() ==
        td::json(R"({"a": 1, "b": [true, "xy"]})"));
  CHECK_STATUS(td::read("smile", "not smile"sv), Status::Decode);
  CHECK_STATUS(td::read("smile", ":)\n\x00\xfa\x80"sv), Status::Decode);
  CHECK_STATUS(td::read("smile", ":)\n\x00\x24\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01"sv), Status::Decode);
  CHECK_STATUS(td::read("smile", ":)\n\x00\x01"sv), Status::Decode);  // shared value without sharing
  Bytes deep = to_bytes(":)\n\x00"sv);
  append(deep, repeat("\xf8", 5000));
  CHECK_STATUS(td::read_bytes("smile", deep), Status::Limit);
  // Writer: header, then the shortest tokens.
  CHECK(hex_of(td::write_bytes("smile", td::json(R"({"a": 1, "b": [true, "xy"]})")).value()) == "3a290a00fa8061c28062f8234178" "79f9fb");
}

TEST_CASE("data/ion: text features") {
  const auto value = td::read("ion", R"($ion_1_0
// comment
{ name: "x", 'quoted field': 1, "str": '''long ''' '''string''', sym: some_symbol, annotated: ann::42,
  nulls: [null, null.int, null.struct], dec: 1.5d0, dec2: 123d-2, flt: 1.5e0, hex: 0x1F, bin: 0b101, under: 1_000,
  ts: [2007T, 2007-02T, 2007-02-23, 2007-02-23T12:14Z, 2007-02-23T12:14:33.079-08:00, 2007-02-23T12:14:00-00:00],
  blob: {{ aGVsbG8= }}, clob: {{ "hi" }}, sexp: (a + b 1), big: 123456789012345678901234567890,
  specials: [nan, +inf, -inf] }
)");
  REQUIRE_OK(value);
  const Json expected = td::json(R"({"name": "x", "quoted field": 1, "str": "long string", "sym": "some_symbol", "annotated": 42,
      "nulls": [null, null, null], "dec": 1.5, "dec2": 1.23, "flt": 1.5, "hex": 31, "bin": 5, "under": 1000,
      "ts": [{"$date": "2007-01-01"}, {"$date": "2007-02-01"}, {"$date": "2007-02-23"}, {"$date": "2007-02-23T12:14:00Z"},
             {"$date": "2007-02-23T12:14:33.079-08:00"}, {"$date": "2007-02-23T12:14:00Z"}],
      "blob": {"$binary": "aGVsbG8="}, "clob": {"$binary": "aGk="}, "sexp": ["a", "+", "b", 1],
      "big": {"$bigint": "123456789012345678901234567890"},
      "specials": [{"$float": "NaN"}, {"$float": "Infinity"}, {"$float": "-Infinity"}]})");
  CHECK_MESSAGE(test::data_equal(*value, expected), test::data_diff(*value, expected));
  CHECK_STATUS(td::read("ion", "{a: 01}"), Status::Decode);
  CHECK_STATUS(td::read("ion", "[1, 2"), Status::Decode);
  CHECK_STATUS(td::read_bytes("ion", hex("e00100ea2e")), Status::Decode);        // length overrun
  CHECK_STATUS(td::read_bytes("ion", hex("e00100eaf0")), Status::Decode);        // reserved type
  CHECK(td::read_bytes("ion", hex("e00100ea0f")).value().is_null());
  CHECK(td::write("ion", td::json(R"({"a": 1.5, "b": {"$decimal": "1e-30"}, "c": {"$date": "2024-05-01T12:00:00"}, "if": [true]})"),
                  R"({"minify": true})").value() == "{a:1.5e0,b:1d-30,c:2024-05-01T12:00:00-00:00,if:[true]}\n");
}

TEST_CASE("data/bencode: strict syntax") {
  CHECK(td::read("bencode", "d3:bar4:spam3:fooi42ee").value() == td::json(R"({"bar": "spam", "foo": 42})"));
  CHECK(td::read("bencode", "li-3ei123456789012345678901234567890ee").value() ==
        td::json("[-3, 123456789012345678901234567890]"));
  CHECK(td::read("bencode", "4:\x00\x01\x02\x03"sv).value() == td::json(R"({"$binary": "AAECAw=="})"));
  CHECK(td::read("bencode", "4:spam", R"({"binary_strings": "always"})").value() == td::json(R"({"$binary": "c3BhbQ=="})"));
  for (const std::string_view bad : {"i-0e", "i03e", "3:ab", "l", "d1:ae", "x", "i1ei2e", "di1ei2ee"}) {
    INFO(bad);
    CHECK_STATUS(td::read("bencode", bad), Status::Decode);
  }
  CHECK(td::write("bencode", td::json(R"({"b": 1, "a": [null, "x"], "c": 1.5})")).value() == "d1:al1:xe1:bi1e1:c3:1.5e");
}

TEST_CASE("data/avro: container errors") {
  CHECK_STATUS(td::read("avro", "Obj"), Status::Decode);
  CHECK_STATUS(td::read("avro", "NotAvro"), Status::Decode);
  const auto file = td::write_bytes("avro", td::json(R"([{"a": 1}, {"a": 2}])"), R"({"avro_codec": "null"})");
  REQUIRE_OK(file);
  Bytes broken = *file;
  broken.back() = static_cast<std::byte>(std::to_integer<unsigned>(broken.back()) ^ 0xFF);  // sync marker
  CHECK_STATUS(td::read_bytes("avro", broken), Status::Decode);
  const auto compressed = td::write_bytes("avro", td::json(R"([{"text": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])"));
  const auto stored = td::write_bytes("avro", td::json(R"([{"text": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])"),
                                      R"({"avro_codec": "null"})");
  CHECK(compressed.value().size() < stored.value().size());
  test::fuzz_reader<ir::Data>("avro", *file);
}

TEST_CASE("data/protobuf: guessing and keeping wire types") {
  // field 1 varint 150, field 2 string "testing", field 3 fixed32 1.5f, field 4 nested {1: 1}
  const Bytes message = hex("089601120774657374696e671d0000c03f22020801");
  CHECK(td::read_bytes("pb", message).value() == td::json(R"({"1": 150, "2": "testing", "3": 1.5, "4": {"1": 1}})"));
  const auto typed = td::read_bytes("pb", message, R"({"typed_keys": true})");
  REQUIRE_OK(typed);
  CHECK(*typed == td::json(R"({"1:varint": 150, "2:string": "testing", "3:float": 1.5, "4:message": {"1:varint": 1}})"));
  CHECK(td::write_bytes("pb", *typed).value() == message);
  CHECK(td::read_bytes("pb", message, R"({"fixed_as": "int"})").value()["3"] == Json(1069547520));
  CHECK(td::read_bytes("pb", hex("080108020803")).value() == td::json(R"({"1": [1, 2, 3]})"));
  CHECK(td::read_bytes("pb", hex("0a0300ff01")).value() == td::json(R"({"1": {"$binary": "AP8B"}})"));
  CHECK_STATUS(td::read_bytes("pb", hex("0a05ff")), Status::Decode);
  CHECK_STATUS(td::read_bytes("pb", hex("ffffffffffffffffffffff01")), Status::Decode);
  CHECK_STATUS(td::write_bytes("pb", td::json(R"({"name": 1})")), Status::Encode);
  CHECK(td::write_bytes("pb", td::json(R"({"1:sint": -1, "2:double": 2, "3": [1, 2]})")).value() ==
        hex("08011100000000000000401801" "1802"));
}

TEST_CASE("data/flexbuffers: hostile input") {
  CHECK_STATUS(td::read_bytes("flexbuffers", hex("0101")), Status::Decode);
  CHECK_STATUS(td::read_bytes("flexbuffers", hex("ff2401")), Status::Decode);   // offset before the buffer
  CHECK_STATUS(td::read_bytes("flexbuffers", hex("0001ff2801")), Status::Decode);  // vector too long
  // A vector whose element points at itself: stopped by the nesting limit.
  CHECK(td::read_bytes("flexbuffers", hex("010000282801")).has_value() == false);
  CHECK(td::read_bytes("flexbuffers", hex("2a0401")).value() == Json(42));
}

TEST_CASE("data/pickle: only plain data is read, nothing is executed") {
  const auto global = td::read("pkl", "cos\nsystem\n(S'echo hi'\ntR.");
  REQUIRE_STATUS(global, Status::Decode);
  CHECK(global.error().message.find("os.system") != std::string::npos);
  CHECK_STATUS(td::read("pkl", "\x80\x04\x95\x10\x00\x00\x00\x00\x00\x00\x00\x8c\x08" "builtins\x8c\x04" "eval\x93."sv),
               Status::Decode);
  CHECK_STATUS(td::read("pkl", "(i__main__\nX\n."), Status::Decode);  // INST
  CHECK_STATUS(td::read("pkl", "]q\x00h\x00" "a."sv), Status::Decode);  // a list containing itself
  CHECK(td::read("pkl", "\x80\x02" "c__builtin__\nset\nq\x00]q\x01(K\x01K\x02" "e\x85q\x02Rq\x03."sv).value() ==
        td::json("[1, 2]"));
  CHECK(td::read("pkl", "(lp0\nI1\naI2\na."sv).value() == td::json("[1, 2]"));
  CHECK(td::read("pkl", "\x80\x02\x8a\x09\x00\x00\x00\x00\x00\x00\x00\x00\x01."sv).value() ==
        td::json(R"({"$bigint": "18446744073709551616"})"));
  CHECK_STATUS(td::read("pkl", "\x80\x05\x97."sv), Status::Decode);  // out-of-band buffer
  CHECK_STATUS(td::read("pkl", "]"), Status::Decode);
}

TEST_CASE("data/bplist: cycles and expansion bombs are rejected") {
  // Two arrays that contain each other.
  Bytes cyclic = to_bytes("bplist00"sv);
  append(cyclic, "\xa1\x01\xa1\x00"sv);  // obj0 = [obj1], obj1 = [obj0]
  append(cyclic, "\x08\x0a"sv);          // offset table
  append(cyclic, std::string_view("\x00\x00\x00\x00\x00\x00\x01\x01"
                                  "\x00\x00\x00\x00\x00\x00\x00\x02"
                                  "\x00\x00\x00\x00\x00\x00\x00\x00"
                                  "\x00\x00\x00\x00\x00\x00\x00\x0c", 32));
  const auto cycle = td::read_bytes("bplist", cyclic);
  REQUIRE_STATUS(cycle, Status::Decode);
  CHECK(cycle.error().message.find("cycle") != std::string::npos);
  // Forty levels of arrays that hold the next level twice: 2^40 values.
  Bytes bomb = to_bytes("bplist00"sv);
  std::vector<std::size_t> offsets;
  for (int i = 0; i < 40; ++i) {
    offsets.push_back(bomb.size());
    append(bomb, std::string_view("\xa2", 1));
    bomb.push_back(static_cast<std::byte>(i + 1));
    bomb.push_back(static_cast<std::byte>(i + 1));
  }
  offsets.push_back(bomb.size());
  append(bomb, std::string_view("\x10\x01", 2));
  const std::size_t table = bomb.size();
  for (const std::size_t offset : offsets) bomb.push_back(static_cast<std::byte>(offset));
  append(bomb, std::string_view("\x00\x00\x00\x00\x00\x00\x01\x01", 8));
  for (const std::uint64_t word : {std::uint64_t{41}, std::uint64_t{0}, static_cast<std::uint64_t>(table)}) {
    for (int shift = 56; shift >= 0; shift -= 8) bomb.push_back(static_cast<std::byte>((word >> shift) & 0xFF));
  }
  CHECK_STATUS(td::read_bytes("bplist", bomb), Status::Limit);
}

TEST_CASE("data/binary: our own encodings survive truncation and corruption") {
  const Json value = td::json(R"({"s": "text é", "i": [1, -200, 70000, 5000000000], "f": [1.5, 1e300], "n": null,
      "b": {"$binary": "AAEC"}, "d": {"$date": "2024-05-01T12:00:00Z"}, "o": {"x": [{"y": true}]}})");
  for (const std::string_view format : {"msgpack", "cbor", "bson", "ubjson", "bjdata", "smile", "bplist", "avro", "bencode",
                                        "pb"}) {
    INFO("format ", format);
    const Json input = format == "bencode" || format == "pb" ? td::json(R"({"1": "text", "2": [1, 2], "3": {"4": "x"}})") : value;
    const auto bytes = td::write_bytes(format, input);
    REQUIRE_OK(bytes);
    CHECK(test::fuzz_reader<ir::Data>(format, *bytes) > 0);
  }
  const auto ion_binary = td::read_bytes("ion", test::fixture("data/ion_python.10n"));
  CHECK_OK(ion_binary);
}
