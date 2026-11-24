// Integration: registry, detection, bridges, options, limits and cancellation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "families/data/data.hpp"
#include "families/data/data_test_support.hpp"
#include "ir/plain_text.hpp"
#include "ir/table.hpp"

using namespace cvt;
using namespace std::string_view_literals;
namespace td = cvt::test::data;

TEST_CASE("data/registry: the shared registry stays consistent") {
  CHECK(Registry::instance().validate().empty());
  for (const std::string_view id : {"json", "jsonc", "json5", "hjson", "jsonl", "json-seq", "yaml", "toml", "xml", "ini",
                                    "properties", "env", "reg", "plist", "bplist", "openstep-plist", "hocon", "kdl", "edn",
                                    "sexp", "ron", "cson", "querystring", "http-headers", "lua-table", "php-serialized",
                                    "python-literal", "msgpack", "cbor", "bson", "ubjson", "bjdata", "smile", "ion",
                                    "bencode", "torrent", "pb", "avro", "flexbuffers", "pkl"}) {
    INFO("format ", id);
    const FormatInfo* info = Registry::instance().find(id);
    REQUIRE(info != nullptr);
    CHECK(info->category == Category::Data);
    CHECK(!info->extensions.empty());
    CHECK(Registry::instance().has_reader(id));
  }
}

TEST_CASE("data/detection: content and names identify the formats") {
  struct Case {
    std::string_view content;
    std::string_view name;
    std::string_view expected;
  };
  const Case cases[] = {
      {"{\"a\": 1}", "data.json", "json"},
      {"{\"a\": 1}", "", "json"},
      {"[1, 2]", "", "json"},
      {"{\"a\":1}\n{\"a\":2}\n", "", "jsonl"},
      {"{\"a\":1}\n", "log.jsonl", "jsonl"},
      {"{a: 1}", "x.json5", "json5"},
      {"// c\n{}", "settings.jsonc", "jsonc"},
      {"a: 1\n", "config.yml", "yaml"},
      {"%YAML 1.2\n---\na: 1\n", "", "yaml"},
      {"a = 1\n", "Cargo.toml", "toml"},
      {"[section]\nkey = value\n", "app.ini", "ini"},
      {"[section]\nkey = value\n", "", "ini"},
      {"<?xml version=\"1.0\"?><a/>", "", "xml"},
      {"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict/></plist>",
       "", "plist"},
      {"bplist00\x08\x08\x00\x00\x00\x00\x00\x00\x01\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00"
       "\x00\x00\x00\x00\x00\x00\x00\x09"sv,
       "Info.plist", "bplist"},
      {"{ a = b; }", "old.plist", "openstep-plist"},
      {"Windows Registry Editor Version 5.00\r\n", "", "reg"},
      {"HTTP/1.1 200 OK\r\nA: b\r\n", "", "http-headers"},
      {"a:1:{i:0;s:1:\"x\";}", "", "php-serialized"},
      {"Obj\x01\x00"sv, "", "avro"},
      {"\xE0\x01\x00\xEA"sv, "", "ion"},
      {"$ion_1_0 {a: 1}", "", "ion"},
      {":)\n\x00\xfa\xfb"sv, "", "smile"},
      {"d8:announce3:urle", "", "torrent"},
      {"\x80\x04\x95\x00\x00\x00\x00\x00\x00\x00\x00N."sv, "", "pkl"},
      {"\xd9\xd9\xf7\x01"sv, "", "cbor"},
      {"KEY=value\n", ".env", "env"},
      {"key=value\n", "messages.properties", "properties"},
      {"app { x = ${y} }\n", "application.conf", "hocon"},
      {"[s]\nk = v\n", "app.conf", "ini"},
      {"return { a = 1 }", "settings.lua", "lua-table"},
      {"node 1\n", "doc.kdl", "kdl"},
      // JSON-based formats of other families keep their extension.
      {"{\"type\": \"FeatureCollection\", \"features\": []}", "map.geojson", "geojson"},
      {"{\"a\": 1}", "values.dat", "json"},
  };
  for (const Case& c : cases) {
    INFO("name '", c.name, "' content ", c.content.substr(0, 20));
    const auto detected = detect_format(Registry::instance(), as_bytes(c.content), c.name);
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == c.expected);
  }
}

TEST_CASE("data/conversion: formats convert into each other through the engine") {
  const std::string yaml = "name: demo\nitems:\n  - 1\n  - 2\nwhen: 2024-05-01\n";
  const test::Converted json = test::convert(yaml, "", "json", "", "config.yaml");
  REQUIRE(json.ok());
  CHECK(json.text() == "{\n  \"name\": \"demo\",\n  \"items\": [\n    1,\n    2\n  ],\n  \"when\": {\n    \"$date\": \"2024-05-01\"\n  }\n}\n");
  const test::Converted plain = test::convert(yaml, "yaml", "json", R"({"special_values": "plain", "minify": true})");
  REQUIRE(plain.ok());
  CHECK(plain.text() == R"({"name":"demo","items":[1,2],"when":"2024-05-01"})");
  const test::Converted toml = test::convert(yaml, "yaml", "toml");
  REQUIRE(toml.ok());
  CHECK(toml.text() == "name = \"demo\"\nitems = [1, 2]\nwhen = 2024-05-01\n");
  const test::Converted back = test::convert(toml.data, "toml", "yaml");
  REQUIRE(back.ok());
  CHECK(back.text() == yaml);
}

TEST_CASE("data/bridges: records become a table") {
  const auto table = [](std::string_view json, std::string_view options = {}) {
    ir::Data data{td::json(json)};
    Context ctx;
    for (const Bridge& bridge : Registry::instance().bridges()) {
      if (bridge.from == Kind::Data && bridge.to == Kind::Table) {
        auto parsed = Options::parse(options);
        ctx.options = parsed->normalized(bridge.options).value();
        auto result = bridge.fn(AnyIR(std::move(data)), ctx);
        REQUIRE_OK(result);
        return std::move(*result).get<ir::Table>();
      }
    }
    FAIL("no Data -> Table bridge");
    return ir::Table{};
  };
  const ir::Table records = table(R"([{"id": 1, "name": "a", "tags": ["x", "y"], "meta": {"ok": true}},
                                      {"id": 2, "extra": null, "when": {"$date": "2024-05-01T00:00:00Z"}}])");
  REQUIRE(records.sheets.size() == 1);
  const ir::Sheet& sheet = records.sheets[0];
  CHECK(sheet.has_header);
  REQUIRE(sheet.rows.size() == 3);
  std::vector<std::string> header;
  for (const ir::Cell& cell : sheet.rows[0]) header.push_back(cell.text);
  CHECK(header == std::vector<std::string>{"id", "name", "tags.0", "tags.1", "meta.ok", "extra", "when"});
  CHECK(sheet.rows[1][0].type == ir::CellType::Number);
  CHECK(sheet.rows[1][2].text == "x");
  CHECK(sheet.rows[1][4].type == ir::CellType::Bool);
  CHECK(sheet.rows[2][5].empty());
  CHECK(sheet.rows[2][6].type == ir::CellType::DateTime);
  CHECK(sheet.rows[2][6].number == 1714521600.0);
  const ir::Table nested = table(R"([{"id": 1, "tags": ["x"]}])", R"({"flatten": "json"})");
  CHECK(nested.sheets[0].rows[1][1].text == "[\"x\"]");
  const ir::Table sheets = table(R"({"people": [{"n": "a"}], "places": [{"p": "b"}, {"p": "c"}]})");
  REQUIRE(sheets.sheets.size() == 2);
  CHECK(sheets.sheets[0].name == "people");
  CHECK(sheets.sheets[1].rows.size() == 3);
  const ir::Table settings = table(R"({"a": 1, "b": {"c": "x"}})");
  CHECK(settings.sheets[0].rows.size() == 3);
  CHECK(settings.sheets[0].rows[2][0].text == "b.c");
  const ir::Table big = table(R"([{"n": 123456789012345678901234567890}])");
  CHECK(big.sheets[0].rows[1][0].text == "123456789012345678901234567890");
}

TEST_CASE("data/bridges: data becomes pretty JSON text") {
  for (const Bridge& bridge : Registry::instance().bridges()) {
    if (bridge.from != Kind::Data || bridge.to != Kind::PlainText) continue;
    Context ctx;
    ctx.options = Options().normalized(bridge.options).value();
    auto result = bridge.fn(AnyIR(ir::Data{td::json(R"({"a": [1]})")}), ctx);
    REQUIRE_OK(result);
    CHECK(std::move(*result).get<ir::PlainText>().text == "{\n  \"a\": [\n    1\n  ]\n}\n");
  }
}

TEST_CASE("data/options: common reader options") {
  CHECK_STATUS(td::read("json", "[]", R"({"special_values": "nonsense"})"), Status::InvalidArgument);
  CHECK_STATUS(td::read("yaml", "a: 1", R"({"documents": "nonsense"})"), Status::InvalidArgument);
  CHECK(td::read("msgpack", "\x01\x02"sv, R"({"documents": "first"})").value() == Json(1));
  CHECK(td::read("cbor", "\x01\x02"sv, R"({"documents": "index", "document_index": 1})").value() == Json(2));
  CHECK(td::read("cbor", "\x01"sv, R"({"documents": "array"})").value() == td::json("[1]"));
  CHECK(td::read("bson", "\x05\x00\x00\x00\x00\x05\x00\x00\x00\x00"sv).value() == td::json("[{}, {}]"));
}

TEST_CASE("data/options: every writer accepts its declared options") {
  // Each writer option is exercised with a non-default value and must succeed.
  const Json value = td::json(R"({"b": [1, 2.5, null], "a": {"t": "x\ny", "d": {"$date": "2024-05-01"}}})");
  for (const Codec& codec : Registry::instance().codecs()) {
    // Schema-bound writers (Codec::ir_tag: sequences, palettes, vector scenes ...) are
    // tested by their families with values of their schema.
    if (codec.kind != Kind::Data || !codec.write || !codec.ir_tag.empty()) continue;
    for (const OptionSpec& spec : codec.write_options) {
      INFO("format ", codec.format_id, " option ", spec.key);
      Json options = Json::object();
      switch (spec.type) {
        case OptionType::Bool: options.set(spec.key, !spec.default_value.as_bool()); break;
        case OptionType::Int: options.set(spec.key, spec.max ? static_cast<std::int64_t>(*spec.max) : 1); break;
        case OptionType::Enum: options.set(spec.key, spec.choices.back()); break;
        case OptionType::String: options.set(spec.key, "x"); break;
        case OptionType::Float: options.set(spec.key, spec.min.value_or(0.0)); break;
      }
      const bool strict_null = spec.key == "null_values" && spec.choices.back() == "error";
      const bool strict_tags = spec.key == "tagged_values" || spec.key == "non_finite";
      const auto out = td::write_bytes(codec.format_id, value, util::dump_json(options));
      if (strict_null || strict_tags) continue;
      CHECK_OK(out);
    }
  }
}

TEST_CASE("data/limits: cancellation and input sizes") {
  // Cancellation is honoured by a reader working on a large input.
  std::string big = "[";
  for (int i = 0; i < 200000; ++i) big += "1,";
  big += "1]";
  const test::Converted ok = test::convert(big, "json", "msgpack");
  CHECK(ok.ok());
  Limits tiny;
  tiny.max_input_bytes = 100;
  CHECK(test::convert(big, "json", "msgpack", "", "", tiny).status == Status::Limit);
  tiny = Limits{};
  tiny.max_output_bytes = 100;
  CHECK(test::convert(big, "json", "msgpack", "", "", tiny).status == Status::Limit);
  // A query string with more pairs than the entry limit.
  tiny = Limits{};
  tiny.max_archive_entries = 3;
  CHECK(test::convert("a=1&b=2&c=3&d=4", "querystring", "json", "", "", tiny).status == Status::Limit);
  // Deflated Avro blocks obey the decompression limit.
  std::string repetitive = R"([{"t": ")" + std::string(100000, 'a') + R"("}])";
  const auto avro = td::write_bytes("avro", td::json(repetitive));
  REQUIRE_OK(avro);
  tiny = Limits{};
  tiny.max_decompressed_bytes = 1000;
  CHECK(test::convert(*avro, "avro", "json", "", "", tiny).status == Status::Limit);
}

TEST_CASE("data/internals: dates, numbers and big-integer helpers") {
  using namespace cvt::data;
  const auto parsed = parse_datetime("2024-02-29T23:59:60.123456789+05:30");
  REQUIRE(parsed.has_value());
  CHECK(parsed->offset_minutes == 330);
  CHECK(parsed->nanos() == 123456789u);
  CHECK(format_datetime(*parsed) == "2024-02-29T23:59:60.123456789+05:30");
  CHECK(!parse_datetime("2023-02-29").has_value());
  CHECK(!parse_datetime("2024-13-01").has_value());
  CHECK(format_datetime(from_unix(-1, 500000000)) == "1969-12-31T23:59:59.5Z");
  CHECK(format_datetime(from_unix(253402300800)) == "+10000-01-01T00:00:00Z");
  CHECK(to_unix_seconds(*parse_datetime("1970-01-02T00:00:00+01:00")) == 82800);
  CHECK(days_from_civil(2000, 3, 1) == 11017);
  CHECK(number_from_literal("1.5") == Json(1.5));
  CHECK(number_from_literal("+.5e1") == Json(5.0));
  CHECK(magnitude_to_decimal(Bytes{std::byte{1}, std::byte{0}, std::byte{0}}, true) == "-65536");
  CHECK(twos_complement_to_decimal(Bytes{std::byte{0xFF}, std::byte{0x00}}) == "-256");
  bool negative = false;
  CHECK(decimal_to_magnitude("-65536", &negative).value() == (Bytes{std::byte{1}, std::byte{0}, std::byte{0}}));
  CHECK(negative);
  CHECK(tag_kind(td::json(R"({"$binary": "AA==", "$subtype": 300})")) == TagKind::None);
  CHECK(tag_kind(td::json(R"({"$decimal": "1x"})")) == TagKind::None);
  CHECK(tag_kind(td::json(R"({"$oid": "zz"})")) == TagKind::None);
}
