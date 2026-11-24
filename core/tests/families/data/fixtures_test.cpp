// Decoding real files written by independent software (see tests/data/data/SOURCES.md):
// every fixture is read with our reader and compared value by value with the
// expected JSON written by Python's json module from the same synthetic data.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string_view>

#include "engine/detect.hpp"
#include "families/data/data_test_support.hpp"

using namespace cvt;
namespace td = cvt::test::data;

namespace {

struct FixtureCase {
  std::string_view file;
  std::string_view format;
  std::string_view expected;
};

constexpr FixtureCase cases[] = {
    {"python_json_pretty.json", "json", "python_json_pretty.expected.json"},
    {"python_json_ascii.json", "json", "python_json_ascii.expected.json"},
    {"python_json_nan.json", "json", "python_json_nan.expected.json"},
    {"python_lines.ndjson", "ndjson", "python_lines.expected.json"},
    {"json5_python.json5", "json5", "json5_python.expected.json"},
    {"hjson_python.hjson", "hjson", "hjson_python.expected.json"},
    {"pyyaml_default.yaml", "yaml", "pyyaml_default.expected.json"},
    {"pyyaml_flow.yaml", "yaml", "pyyaml_flow.expected.json"},
    {"pyyaml_multi.yaml", "yaml", "pyyaml_multi.expected.json"},
    {"ruamel_default.yaml", "yaml", "ruamel_default.expected.json"},
    {"tomli_w.toml", "toml", "tomli_w.expected.json"},
    {"plistlib_xml.plist", "plist", "plistlib_xml.expected.json"},
    {"plistlib_binary.plist", "bplist", "plistlib_binary.expected.json"},
    {"plutil_xml.plist", "plist", "plutil_xml.expected.json"},
    {"plutil_binary.plist", "bplist", "plutil_binary.expected.json"},
    {"configparser.ini", "ini", "configparser.expected.json"},
    {"jproperties.properties", "properties", "jproperties.expected.json"},
    {"elementtree.xml", "xml", "elementtree.expected.json"},
    {"msgpack_python.msgpack", "msgpack", "msgpack_python.expected.json"},
    {"msgpack_ext.msgpack", "msgpack", "msgpack_ext.expected.json"},
    {"cbor_python.cbor", "cbor", "cbor_python.expected.json"},
    {"cbor_canonical.cbor", "cbor", "cbor_canonical.expected.json"},
    {"bson_pymongo.bson", "bson", "bson_pymongo.expected.json"},
    {"ubjson_python.ubj", "ubjson", "ubjson_python.expected.json"},
    {"ubjson_python_optimised.ubj", "ubjson", "ubjson_python_optimised.expected.json"},
    {"bjdata_python.bjd", "bjdata", "bjdata_python.expected.json"},
    {"jackson_default.sml", "smile", "jackson_smile.expected.json"},
    {"jackson_shared.sml", "smile", "jackson_smile.expected.json"},
    {"ion_python.ion", "ion", "ion_python.expected.json"},
    {"ion_python.10n", "ion", "ion_python.expected.json"},
    {"fastavro_null.avro", "avro", "fastavro_null.expected.json"},
    {"fastavro_deflate.avro", "avro", "fastavro_deflate.expected.json"},
    {"bencode_torrent.torrent", "torrent", "bencode_torrent.expected.json"},
    {"flexbuffers_python.flexbuf", "flexbuffers", "flexbuffers_python.expected.json"},
    {"pickle_protocol0.pickle", "pickle", "pickle.expected.json"},
    {"pickle_protocol1.pickle", "pickle", "pickle.expected.json"},
    {"pickle_protocol2.pickle", "pickle", "pickle.expected.json"},
    {"pickle_protocol3.pickle", "pickle", "pickle.expected.json"},
    {"pickle_protocol4.pickle", "pickle", "pickle.expected.json"},
    {"pickle_protocol5.pickle", "pickle", "pickle.expected.json"},
    {"edn_python.edn", "edn", "edn_python.expected.json"},
    {"php_serialize.txt", "php-serialized", "php_serialize.expected.json"},
    {"python_repr.pyon", "python-literal", "python_repr.expected.json"},
    {"kdl_python.kdl", "kdl", "kdl_python.expected.json"},
    {"sexpdata.sexp", "sexp", "sexpdata.expected.json"},
    {"pyhocon.conf", "hocon", "pyhocon.expected.json"},
};

}  // namespace

TEST_CASE("data/fixtures: files written by independent software decode to their known content") {
  for (const FixtureCase& c : cases) {
    INFO("fixture ", c.file, " as ", c.format);
    const Bytes input = test::fixture(std::string("data/") + std::string(c.file));
    const Json expected = td::json(test::fixture_text(std::string("data/") + std::string(c.expected)));
    const auto decoded = td::read_bytes(c.format, input);
    REQUIRE_OK(decoded);
    CHECK_MESSAGE(test::data_equal(*decoded, expected), test::data_diff(*decoded, expected));
  }
}

TEST_CASE("data/fixtures: every fixture is detected as its format") {
  // Content and file name together, as the apps see them.
  for (const FixtureCase& c : cases) {
    INFO("fixture ", c.file);
    if (c.format == "php-serialized" || c.format == "sexp") continue;  // no extension of their own in the fixture names
    const Bytes input = test::fixture(std::string("data/") + std::string(c.file));
    const auto detected = detect_format(Registry::instance(), input, c.file);
    REQUIRE(detected.has_value());
    const std::string_view id = detected->format->id;
    const bool plist_family = (c.format == "plist" || c.format == "bplist") && (id == "plist" || id == "bplist");
    CHECK_MESSAGE(id == c.format || plist_family || (c.format == "torrent" && id == "torrent"), "detected ", id);
  }
}

TEST_CASE("data/fixtures: truncated and corrupted copies of every fixture never crash") {
  for (const FixtureCase& c : cases) {
    INFO("fixture ", c.file);
    const Bytes input = test::fixture(std::string("data/") + std::string(c.file));
    CHECK(test::fuzz_reader<ir::Data>(c.format, input, 128) > 0);
  }
}

TEST_CASE("data/fixtures: the CBOR indefinite-length examples decode") {
  // Four RFC 8949 Appendix A style items, written by hand; read as a CBOR sequence.
  const auto decoded = td::read_bytes("cbor", test::fixture("data/cbor_indefinite.cbor"));
  REQUIRE_OK(decoded);
  const Json expected = td::json(R"([{"Fun": true, "Amt": -2}, [1, [2, 3], [4, 5]],
                                     {"$binary": "AQIDBAU="}, "streaming"])");
  CHECK_MESSAGE(test::data_equal(*decoded, expected), test::data_diff(*decoded, expected));
}

TEST_CASE("data/fixtures: a schema-less Protocol Buffers message keeps its fields") {
  // A FileDescriptorProto serialised by the protobuf Python library.
  const auto decoded = td::read_bytes("protobuf", test::fixture("data/protobuf_descriptor.pb"));
  REQUIRE_OK(decoded);
  CHECK((*decoded)["1"].as_string() == "example.proto");
  CHECK((*decoded)["2"].as_string() == "demo");
  const Json& message = (*decoded)["4"];
  CHECK(message["1"].as_string() == "Point");
  REQUIRE(message["2"].is_array());
  CHECK(message["2"].size() == 2);
  CHECK(message["2"][0]["1"].as_string() == "x");
  CHECK(message["2"][0]["3"].as_int64() == 1);   // number
  CHECK(message["2"][0]["5"].as_int64() == 17);  // TYPE_SINT32
  CHECK(message["2"][1]["7"].as_string() == "-1.5");
  // Encoding the tree again gives the same bytes.
  const auto encoded = td::write_bytes("protobuf", *decoded);
  REQUIRE_OK(encoded);
  CHECK(*encoded == test::fixture("data/protobuf_descriptor.pb"));
}
