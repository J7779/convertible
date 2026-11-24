// Configuration and notation formats: INI, properties, .env, .reg, query
// strings, HTTP headers, XML mapping, plists (OpenStep), HOCON, KDL, EDN,
// S-expressions, RON, CSON, Lua, PHP and Python literals.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/data/data_test_support.hpp"
#include "util/text.hpp"

using namespace cvt;
using namespace std::string_view_literals;
namespace td = cvt::test::data;

namespace {

void expect(std::string_view format, std::string_view text, std::string_view json, std::string_view options = {}) {
  INFO(format, ":\n", text);
  const auto value = td::read(format, text, options);
  REQUIRE_OK(value);
  const Json expected = td::json(json);
  CHECK_MESSAGE(test::data_equal(*value, expected), test::data_diff(*value, expected), "\n got: ", util::dump_json(*value));
}

}  // namespace

TEST_CASE("data/ini: dialects") {
  const std::string_view ini = "; comment\nglobal = 1\n[server]\nhost = example.invalid ; inline\nport: 8080\n"
                               "path = \"/a b;c\"\ncolor = #fff\nflag\nlist[] = a\nlist[] = b\n"
                               "[multi]\ntext = first\n  second\n\n[a.b]\nx = 1\n";
  expect("ini", ini, R"({"global": "1", "server": {"host": "example.invalid", "port": "8080", "path": "/a b;c",
      "color": "#fff", "flag": null, "list": ["a", "b"]}, "multi": {"text": "first\nsecond"}, "a.b": {"x": "1"}})");
  expect("ini", ini, R"({"global": 1, "server": {"host": "example.invalid", "port": 8080, "path": "/a b;c",
      "color": "#fff", "flag": null, "list": ["a", "b"]}, "multi": {"text": "first\nsecond"}, "a": {"b": {"x": 1}}})",
         R"({"infer_types": true, "nested_sections": true})");
  expect("ini", "[s]\nk = v ; not a comment\n", R"({"s": {"k": "v ; not a comment"}})", R"({"inline_comments": false})");
  expect("ini", "[s]\nk = 'q'\n", R"({"s": {"k": "'q'"}})", R"({"unquote": false})");
  expect("ini", "[core]\n\trepositoryformatversion = 0\n\tbare = false\n", R"({"core": {"repositoryformatversion": "0", "bare": "false"}})");
  CHECK_STATUS(td::read("ini", "[unclosed\n"), Status::Decode);
  CHECK_STATUS(td::read("ini", "= value\n"), Status::Decode);
  // Writer.
  const Json value = td::json(R"({"top": 1, "s": {"a": "x", "list": [1, 2], "nested": {"k": true}, "n": null, "sp": " x"}})");
  CHECK(td::write("ini", value).value() == "top = 1\n\n[s]\na = x\nlist[] = 1\nlist[] = 2\nnested.k = true\nn = \nsp = \" x\"\n");
  CHECK(td::write("ini", value, R"({"separator": "=", "array_style": "json", "null_values": "omit", "line_ending": "crlf"})").value() ==
        "top=1\r\n\r\n[s]\r\na=x\r\nlist=[1,2]\r\nnested={\"k\":true}\r\nsp=\" x\"\r\n");
  CHECK(td::write("ini", value, R"({"array_style": "indexed", "null_values": "bare_key"})").value().find("list.0 = 1\nlist.1 = 2\n") !=
        std::string::npos);
}

TEST_CASE("data/properties: escapes, continuations and encodings") {
  expect("properties", "# c\n! c\nkey = value\nkey2:value2\nkey3 value3\nspaced\\ key = x\nuni = \\u00e9\\uD83D\\uDE00\n"
                       "multi = one \\\n    two\nempty\ntab = a\\tb\n",
         R"({"key": "value", "key2": "value2", "key3": "value3", "spaced key": "x", "uni": "\u00e9\ud83d\ude00",
             "multi": "one two", "empty": "", "tab": "a\tb"})");
  expect("properties", "latin = caf\xe9\n", R"({"latin": "caf\u00e9"})");  // ISO-8859-1 bytes
  expect("properties", "a.b = 1\na.c = 2\n", R"({"a": {"b": "1", "c": "2"}})", R"({"nested_keys": true})");
  CHECK(td::write("properties", td::json(R"({"a": {"b": "x y"}, "k=1": "\u00e9\n", "lead": " s"})")).value() ==
        "a.b=x y\nk\\=1=\\u00E9\\n\nlead=\\ s\n");
  CHECK(td::write("properties", td::json(R"({"u": "\u00e9"})"), R"({"ascii_only": false, "separator": " = "})").value() ==
        "u = \xC3\xA9\n");
  CHECK_STATUS(td::read("properties", "a = \\u12"), Status::Decode);
}

TEST_CASE("data/env: dotenv files") {
  expect("env", "# comment\nexport A=1\nB = two words # comment\nC=\"quoted \\n \\\"x\\\"\"\nD='single $A'\nE=\"multi\nline\"\nF=\n",
         R"({"A": "1", "B": "two words", "C": "quoted \n \"x\"", "D": "single $A", "E": "multi\nline", "F": ""})");
  expect("env", "A=1\nB=${A}2\nC=\"$A-x\"\n", R"({"A": "1", "B": "12", "C": "1-x"})", R"({"expand_variables": true})");
  CHECK(td::write("env", td::json(R"({"app": {"name": "my app", "port": 80}, "list": [1, 2], "q": "a\"b$"})")).value() ==
        "app_name=\"my app\"\napp_port=80\nlist_0=1\nlist_1=2\nq=\"a\\\"b\\$\"\n");
  CHECK(td::write("env", td::json(R"({"key": "v"})"), R"({"uppercase_keys": true, "export": true})").value() == "export KEY=v\n");
  CHECK_STATUS(td::read("env", "NOT VALID\n"), Status::Decode);
  CHECK_STATUS(td::read("env", "A=\"open\n"), Status::Decode);
}

TEST_CASE("data/reg: registry files") {
  const std::string_view reg = "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Demo]\r\n"
                               "@=\"default\"\r\n\"Name\"=\"quote \\\" back \\\\\"\r\n\"Count\"=dword:0000001f\r\n"
                               "\"Bin\"=hex:00,01,\\\r\n  ff\r\n\"Multi\"=hex(7):61,00,00,00,62,00,00,00,00,00\r\n"
                               "\"Expand\"=hex(2):25,00,41,00,25,00,00,00\r\n\"Q\"=hex(b):01,00,00,00,00,00,00,00\r\n"
                               "\"Gone\"=-\r\n\r\n[-HKEY_CURRENT_USER\\Software\\Old]\r\n";
  const Json expected = td::json(R"({"HKEY_CURRENT_USER\\Software\\Demo": {"@": "default", "Name": "quote \" back \\",
      "Count": 31, "Bin": {"$binary": "AAH/"}, "Multi": ["a", "b"], "Expand": {"type": "REG_EXPAND_SZ", "value": "%A%"},
      "Q": {"type": "REG_QWORD", "value": 1}, "Gone": null}, "-HKEY_CURRENT_USER\\Software\\Old": null})");
  expect("reg", reg, util::dump_json(expected));
  // Written as UTF-16 with a BOM and read back identically.
  const auto bytes = td::write_bytes("reg", expected);
  REQUIRE_OK(bytes);
  CHECK(to_string(ByteView(*bytes).first(2)) == "\xFF\xFE");
  CHECK(td::read_bytes("reg", *bytes).value() == expected);
  const auto utf8 = td::write("reg", expected, R"({"reg_encoding": "utf-8"})");
  REQUIRE_OK(utf8);
  CHECK(utf8->starts_with("Windows Registry Editor Version 5.00\r\n"));
  CHECK(td::read("reg", *utf8).value() == expected);
  CHECK_STATUS(td::read("reg", "[HKEY_X]\r\n"), Status::Decode);
  CHECK_STATUS(td::read("reg", "REGEDIT4\r\n\"a\"=1\r\n"), Status::Decode);
}

TEST_CASE("data/querystring: nested keys") {
  expect("querystring", "?a=1&b[]=x&b[]=y&c[d][e]=2&f=1&f=2&g&h=%C3%A9+%26&i[0][k]=1&i[1][k]=2#frag",
         R"({"a": "1", "b": ["x", "y"], "c": {"d": {"e": "2"}}, "f": ["1", "2"], "g": null, "h": "\u00e9 &",
             "i": [{"k": "1"}, {"k": "2"}]})");
  expect("querystring", "a.b=1&a.c=2", R"({"a": {"b": "1", "c": "2"}})", R"({"nested_keys": "dots"})");
  expect("querystring", "a[b]=1", R"({"a[b]": "1"})", R"({"nested_keys": "none"})");
  expect("querystring", "a=1;b=2", R"({"a": "1", "b": "2"})", R"({"semicolon_separator": true})");
  expect("querystring", "bad=%zz", R"({"bad": "%zz"})");
  const Json value = td::json(R"({"a": "x y", "b": [1, 2], "c": {"d": [{"e": true}]}})");
  CHECK(td::write("querystring", value).value() == "a=x+y&b[]=1&b[]=2&c[d][0][e]=true\n");
  CHECK(td::write("querystring", value, R"({"array_format": "indices", "nested_keys": "dots"})").value() ==
        "a=x+y&b.0=1&b.1=2&c.d.0.e=true\n");
  CHECK(td::write("querystring", td::json(R"({"b": [1, 2]})"), R"({"array_format": "repeat"})").value() == "b=1&b=2\n");
}

TEST_CASE("data/http-headers: header blocks") {
  expect("http-headers", "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
                         "X-Folded: one\r\n  two\r\n\r\nbody",
         R"({":start-line": "HTTP/1.1 200 OK", "Content-Type": "text/plain", "Set-Cookie": ["a=1", "b=2"], "X-Folded": "one two"})");
  CHECK(td::write("http-headers", td::json(R"({":start-line": "GET / HTTP/1.1", "Host": "example.invalid", "Accept": ["a", "b"]})"))
            .value() == "GET / HTTP/1.1\r\nHost: example.invalid\r\nAccept: a\r\nAccept: b\r\n");
  CHECK_STATUS(td::read("http-headers", "Name: v\r\nnot a header\r\n"), Status::Decode);
}

TEST_CASE("data/xml: mapping conventions") {
  const std::string_view xml = "<?xml version=\"1.0\"?>\n<root xmlns:x=\"urn:x\" id=\"7\">\n  <item>a</item>\n  <item>b</item>\n"
                               "  <x:node attr=\"1\">text</x:node>\n  <empty/>\n  <mixed>t1<b>bold</b>t2</mixed>\n"
                               "  <num>42</num>\n  <![CDATA[ignored? no]]>\n</root>\n";
  expect("xml", xml, R"({"root": {"@xmlns:x": "urn:x", "@id": "7", "item": ["a", "b"], "x:node": {"@attr": "1", "#text": "text"},
      "empty": null, "mixed": {"b": "bold", "#text": "t1t2"}, "num": "42", "#text": "ignored? no"}})");
  expect("xml", "<r a=\"1\"><n>2.5</n><t>true</t><s>007</s></r>", R"({"r": {"_a": 1, "n": 2.5, "t": true, "s": "007"}})",
         R"({"infer_types": true, "attribute_prefix": "_"})");
  expect("xml", "<r><one>x</one></r>", R"({"r": {"one": ["x"]}})", R"({"force_arrays": true})");
  expect("xml", "<r><t>  padded  </t></r>", R"({"r": {"t": "  padded  "}})", R"({"strip_whitespace": false})");
  // Writer.
  const Json value = td::json(R"({"root": {"@id": "7", "item": ["a", "b"], "child": {"#text": "t", "@x": "1"},
      "bad key": 1, "n": null, "list": [[1, 2]], "bin": {"$binary": "AAE="}}})");
  std::vector<std::string> warnings;
  const auto text = td::write("xml", value, {}, &warnings);
  REQUIRE_OK(text);
  CHECK(*text == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root id=\"7\">\n  <item>a</item>\n  <item>b</item>\n"
                 "  <child x=\"1\">t</child>\n  <bad_key>1</bad_key>\n  <n/>\n  <list>\n    <item>1</item>\n    <item>2</item>\n"
                 "  </list>\n  <bin>AAE=</bin>\n</root>\n");
  CHECK(warnings.size() == 1);
  CHECK(td::write("xml", td::json("[1, 2]"), R"({"indent": 0, "root_name": "data"})").value() ==
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><data><item>1</item><item>2</item></data>\n");
  CHECK_STATUS(td::read("xml", "<a><b></a>"), Status::Decode);
  CHECK_STATUS(td::read("xml", "<!DOCTYPE a [<!ENTITY e SYSTEM \"file:///etc/passwd\">]><a>&e;</a>"), Status::Decode);
}

TEST_CASE("data/plist: OpenStep and .strings files") {
  expect("openstep-plist", "// comment\n{ name = \"Demo App\"; version = 1.2; list = (a, \"b c\", <0001ff>); nested = { k = v; }; "
                           "esc = \"tab\\tq\\\"\\U00e9\"; }",
         R"({"name": "Demo App", "version": "1.2", "list": ["a", "b c", {"$binary": "AAH/"}], "nested": {"k": "v"},
             "esc": "tab\tq\"\u00e9"})");
  expect("openstep-plist", "/* strings */\n\"greeting\" = \"Hello\";\n\"bye\" = \"Bye\";\n", R"({"greeting": "Hello", "bye": "Bye"})");
  expect("openstep-plist", "{ i = <*I42>; r = <*R1.5>; b = <*BY>; d = <*D2024-05-01 12:30:00 +0000>; }",
         R"({"i": 42, "r": 1.5, "b": true, "d": {"$date": "2024-05-01T12:30:00Z"}})");
  CHECK_STATUS(td::read("openstep-plist", "{ a = 1"), Status::Decode);
}

TEST_CASE("data/hocon: substitutions, merging and paths") {
  expect("hocon", "# comment\na.b.c = 1\na { d = 2 }\nname = demo\ngreeting = \"hello \"${name}\" !\"\nlist = [1, 2]\n"
                  "list += 3\nopt = ${?missing}\nobj = { x = 1 } { y = 2 }\nmulti = \"\"\"raw \"text\"\"\"\"\n"
                  "unquoted = some text here\nnum = 1.5\nbool = true\nnull_value = null\ninclude \"other.conf\"\n",
         R"({"a": {"b": {"c": 1}, "d": 2}, "name": "demo", "greeting": "hello demo !", "list": [1, 2, 3], "opt": null,
             "obj": {"x": 1, "y": 2}, "multi": "raw \"text\"", "unquoted": "some text here", "num": 1.5, "bool": true,
             "null_value": null})");
  const auto missing = td::read("hocon", "a = ${nope}\n");
  REQUIRE_STATUS(missing, Status::Decode);
  CHECK(missing.error().message.find("nope") != std::string::npos);
  CHECK_STATUS(td::read("hocon", "a = ${b}\nb = ${a}\n"), Status::Decode);
}

TEST_CASE("data/kdl: nodes, both syntax versions") {
  expect("kdl", "// comment\ntitle \"Hello\"\ncount 3\nflags #true #false #null\nold true null\n"
                "server host=\"x.invalid\" port=8080 {\n    path \"/srv\"\n}\nitems {\n    - 1\n    - two\n}\n"
                "/-skipped 1\nraw #\"a\\b\"#\nv1raw r\"c\\d\"\nmulti \"\"\"\n    line1\n    line2\n    \"\"\"\n"
                "(array)one 5\nempty {}\nnums 0x1F 1_000 1.5e3 #inf\nrep 1\nrep 2\n",
         R"({"title": "Hello", "count": 3, "flags": [true, false, null], "old": [true, null],
             "server": {"host": "x.invalid", "port": 8080, "path": "/srv"}, "items": [1, "two"], "raw": "a\\b",
             "v1raw": "c\\d", "multi": "line1\nline2", "one": [5], "empty": {}, "nums": [31, 1000, 1500.0, {"$float": "Infinity"}],
             "rep": [1, 2]})");
  const Json value = td::json(R"({"name": "x y", "list": [1, 2], "single": [1], "none": [], "obj": {"a": null},
      "mixed": [1, {"k": true}], "if": "true"})");
  const auto text = td::write("kdl", value);
  REQUIRE_OK(text);
  CHECK(*text == "name \"x y\"\nlist 1 2\n(array)single 1\n(array)none\nobj {\n    a #null\n}\n"
                 "(array)mixed {\n    - 1\n    - {\n        k #true\n    }\n}\nif \"true\"\n");
  CHECK(td::read("kdl", *text).value() == value);
  CHECK_STATUS(td::read("kdl", "node \"open\n"), Status::Decode);
  CHECK_STATUS(td::read("kdl", "node {\n"), Status::Decode);
}

TEST_CASE("data/edn: forms") {
  expect("edn", "; comment\n{:name \"x\" :tags #{:a :b} :n 42N :d 1.5M :f 1.5 :nil nil :chars [\\a \\newline \\u00e9]\n"
                " :inst #inst \"2024-05-01T12:00:00Z\" :uuid #uuid \"f81d4fae-7dec-11d0-a765-00a0c91e6bf6\" #_ :ignored :list (1 2)\n"
                " \"string key\" sym/bol :inf ##Inf}",
         R"({"name": "x", "tags": ["a", "b"], "n": 42, "d": {"$decimal": "1.5"}, "f": 1.5, "nil": null, "chars": ["a", "\n", "\u00e9"],
             "inst": {"$date": "2024-05-01T12:00:00Z"}, "uuid": "f81d4fae-7dec-11d0-a765-00a0c91e6bf6", "list": [1, 2],
             "string key": "sym/bol", "inf": {"$float": "Infinity"}})");
  expect("edn", "{:a 1}", R"({":a": 1})", R"({"keywords": "keep"})");
  CHECK(td::write("edn", td::json(R"({"a": 1, "b c": [true, null], "big": {"$bigint": "123456789012345678901234567890"},
      "dec": {"$decimal": "0.1000000000000000000001"}})")).value() ==
        "{:a 1\n \"b c\" [true nil]\n :big 123456789012345678901234567890N\n :dec 0.1000000000000000000001M}\n");
  CHECK(td::write("edn", td::json(R"({"a": 1})"), R"({"keyword_keys": false})").value() == "{\"a\" 1}\n");
  CHECK_STATUS(td::read("edn", "{:a}"), Status::Decode);
  CHECK_STATUS(td::read("edn", "(1 2"), Status::Decode);
}

TEST_CASE("data/sexp: lists and association lists") {
  expect("sexp", "; comment\n((name \"demo\") (count . 3) (tags (a b)) (flags (t nil)))", R"({"name": "demo", "count": 3, "tags": ["a", "b"],
      "flags": [true, null]})");
  expect("sexp", "((a 1) (b 2))", R"([["a", 1], ["b", 2]])", R"({"alists": false})");
  expect("sexp", "(1 2.5 \"s\" #t #f :key 'quoted)", R"([1, 2.5, "s", true, false, "key", "quoted"])");
  CHECK(td::write("sexp", td::json(R"({"a": [1, "x"], "b c": true})")).value() == "((a (1 \"x\"))\n (\"b c\" #t))\n");
  CHECK_STATUS(td::read("sexp", "(1 2"), Status::Decode);
}

TEST_CASE("data/ron: Rust values") {
  expect("ron", "#![enable(implicit_some)]\n// comment\nGame(\n  title: \"Demo\",\n  size: (800, 600),\n  mode: Windowed,\n"
                "  opt: Some(5),\n  none: None,\n  unit: (),\n  map: {\"a\": 1, 2: \"b\"},\n  list: [1.5, 0x1F, 1_000u32],\n"
                "  ch: 'c',\n  raw: r#\"no \\escape\"#,\n  bytes: b\"\\x00\\x01\",\n  variant: Move(1, 2),\n  single: Wrap(1),\n"
                "  st: Point(x: 1, y: 2),\n  ok: true,\n)",
         R"({"title": "Demo", "size": [800, 600], "mode": "Windowed", "opt": 5, "none": null, "unit": null,
             "map": {"a": 1, "2": "b"}, "list": [1.5, 31, 1000], "ch": "c", "raw": "no \\escape", "bytes": {"$binary": "AAE="},
             "variant": {"Move": [1, 2]}, "single": {"Wrap": 1}, "st": {"x": 1, "y": 2}, "ok": true})");
  CHECK_STATUS(td::read("ron", "(a: 1"), Status::Decode);
}

TEST_CASE("data/cson: indentation and brackets") {
  expect("cson", "# comment\nname: 'demo'\nversion: 1.5\nenabled: yes\nnested:\n  deep:\n    value: \"x\"\n"
                 "list: [\n  1\n  2\n]\nobjects: [\n  a: 1\n  b: 2\n,\n  a: 3\n]\ninline: {x: 1, y: [true, null]}\n"
                 "multi: '''\n  line1\n  line2\n'''\n",
         R"({"name": "demo", "version": 1.5, "enabled": true, "nested": {"deep": {"value": "x"}}, "list": [1, 2],
             "objects": [{"a": 1, "b": 2}, {"a": 3}], "inline": {"x": 1, "y": [true, null]}, "multi": "line1\nline2"})");
  CHECK_STATUS(td::read("cson", "a: 1\n    b: 2\n"), Status::Decode);
}

TEST_CASE("data/lua-table: tables") {
  expect("lua-table", "-- settings\nreturn {\n  name = \"demo\", ['key with space'] = 1, [10] = 'ten',\n  list = { 1, 2, 3 },\n"
                      "  long = [[raw\nstring]], esc = \"tab\\tnl\\n\\65\\u{E9}\", flag = true, none = nil, neg = -1.5e2, hex = 0x1F,\n"
                      "  --[[ block ]] inf = math.huge\n}",
         R"({"name": "demo", "key with space": 1, "10": "ten", "list": [1, 2, 3], "long": "raw\nstring",
             "esc": "tab\tnl\nA\u00e9", "flag": true, "none": null, "neg": -150.0, "hex": 31, "inf": {"$float": "Infinity"}})");
  expect("lua-table", "config = { 'a', 'b' }", R"(["a", "b"])");
  CHECK(td::write("lua-table", td::json(R"({"a": 1, "end": [true, "x"], "n": {"$float": "NaN"}})")).value() ==
        "return {\n  a = 1,\n  [\"end\"] = {\n    true,\n    \"x\",\n  },\n  n = (0/0),\n}\n");
  CHECK_STATUS(td::read("lua-table", "{ a = os.execute('x') }"), Status::Decode);
}

TEST_CASE("data/php-serialized: values, objects and references") {
  expect("php-serialized", "a:4:{s:1:\"a\";i:1;s:1:\"b\";a:2:{i:0;s:1:\"x\";i:1;d:0.5;}s:1:\"o\";O:8:\"stdClass\":2:{"
                           "s:4:\"\0*\0p\";b:1;s:10:\"\0Demo\0priv\";N;}s:1:\"r\";R:3;}"sv,
         R"({"a": 1, "b": ["x", 0.5], "o": {"p": true, "priv": null}, "r": ["x", 0.5]})");
  expect("php-serialized", "d:INF;", R"({"$float": "Infinity"})");
  expect("php-serialized", R"(s:3:"€";)", R"("\u20ac")");  // byte length 3
  CHECK(td::write("php-serialized", td::json(R"({"a": [1, "x"], "5": true, "f": 1.5, "n": null})")).value() ==
        R"(a:4:{s:1:"a";a:2:{i:0;i:1;i:1;s:1:"x";}i:5;b:1;s:1:"f";d:1.5;s:1:"n";N;})");
  CHECK_STATUS(td::read("php-serialized", R"(s:10:"short";)"), Status::Decode);
  CHECK_STATUS(td::read("php-serialized", R"(a:999999999:{)"), Status::Decode);
  CHECK_STATUS(td::read("php-serialized", "R:5;"), Status::Decode);
}

TEST_CASE("data/python-literal: literal_eval syntax") {
  expect("python-literal", "# comment\n{'a': 1, \"b\": [True, False, None], 'c': (1, 2), 'd': {1, 2}, 'e': b'\\x00\\xff',\n"
                           " 'f': 'con' 'cat', 'g': r'raw\\n', 'h': '''tri\nple''', 'i': 1_000, 'j': 0x1F, 'k': -1.5e3,\n"
                           " 'l': float('inf'), 'm': 12345678901234567890123, 3: 'int key', 'n': set(), 'o': u'\\u00e9'}",
         R"({"a": 1, "b": [true, false, null], "c": [1, 2], "d": [1, 2], "e": {"$binary": "AP8="}, "f": "concat",
             "g": "raw\\n", "h": "tri\nple", "i": 1000, "j": 31, "k": -1500.0, "l": {"$float": "Infinity"},
             "m": {"$bigint": "12345678901234567890123"}, "3": "int key", "n": [], "o": "\u00e9"})");
  CHECK_STATUS(td::read("python-literal", "__import__('os').system('x')"), Status::Decode);
  CHECK_STATUS(td::read("python-literal", "{'a': 1"), Status::Decode);
  CHECK(td::write("python-literal", td::json(R"({"a": "it's", "b": [1.5, null, true], "c": {"$binary": "AP8="}})"),
                  R"({"minify": true})").value() == "{'a': \"it's\", 'b': [1.5, None, True], 'c': b'\\x00\\xff'}\n");
}
