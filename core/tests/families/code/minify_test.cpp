// Tests of the token-level minifiers and pretty-printers (minify.cpp).
// The inputs in tests/data/code/minify/ are hand-written edge cases; the
// outputs were checked with independent tools while developing (node --check
// and running both versions, tinycss2, sqlglot, graphql-core, Python's
// html.parser; see SOURCES.md).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "families/code/minify.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using test::convert;

namespace {
std::string input(std::string_view name) { return test::fixture_text("code/minify/" + std::string(name)); }
}  // namespace

TEST_CASE("code/js-min: regexes, templates, ASI and number edge cases") {
  const std::string js = input("tricky.js");
  const auto min = code::minify_js(js);
  REQUIRE_OK(min);
  CHECK(*min == input("tricky.expected.min.js"));
  CHECK(min->size() < js.size() * 7 / 10);
  // Idempotent, and pretty-printing then minifying gives the same result.
  CHECK(*code::minify_js(*min) == *min);
  CHECK(code::minify_js(*code::pretty_js(js))->size() <= min->size() + 8);  // only line breaks may remain
  CHECK(min->find("Licence banner: kept") != std::string::npos);
  CHECK(code::minify_js(js, {false})->find("Licence banner") == std::string::npos);
  // Small cases.
  CHECK(*code::minify_js("a = b\n/c/g.test(d)") == "a=b/c/g.test(d)");       // division, not a regex
  CHECK(*code::minify_js("x = 1;\n/re/.test(s)") == "x=1;/re/.test(s)");     // regex after ';'
  CHECK(*code::minify_js("return\nvalue") == "return\nvalue");                // restricted production
  CHECK(*code::minify_js("a\n++b") == "a\n++b");                              // postfix vs prefix
  CHECK(*code::minify_js("a +\n  b") == "a+b");                               // no ASI after '+'
  CHECK(*code::minify_js("f(a)\n(b)") == "f(a)\n(b)");                         // would become a call
  CHECK(*code::minify_js("a - -b; c + +d; e - --f") == "a- -b;c+ +d;e- --f");
  CHECK(*code::minify_js("1 .toString(); 1.5 .toFixed()") == "1 .toString();1.5.toFixed()");
  CHECK(*code::minify_js("const s = `a ${ `b ${c}` } d` // x") == "const s=`a ${ `b ${c}` } d`");
  CHECK_STATUS(code::minify_js("'unterminated"), Status::Decode);
  CHECK_STATUS(code::minify_js("/* open"), Status::Decode);
  CHECK_STATUS(code::minify_js("`${"), Status::Decode);
}

TEST_CASE("code/js: engine routes, same-format compress and options") {
  const std::string js = "// hi\nfunction f( a ) {\n  return a  +  1 ;\n}\n";
  CHECK(convert(js, "js", "js-min").text() == "function f(a){return a+1;}");
  CHECK(convert(js, "js", "js").text() == "function f(a){return a+1;}");  // compress
  CHECK(convert(js, "js", "js", R"({"style":"pretty"})").text() == "// hi\nfunction f(a) {\n  return a + 1;\n}\n");
  CHECK(convert(js, "js", "js", R"({"style":"keep"})").text() == js);
  CHECK(convert(js, "js", "js-pretty", R"({"indent":4})").text() == "// hi\nfunction f(a) {\n    return a + 1;\n}\n");
}

TEST_CASE("code/css-min and css-pretty: safe transforms only") {
  const std::string css = input("tricky.css");
  const auto min = code::minify_css(css);
  REQUIRE_OK(min);
  CHECK(min->starts_with("/*! keep this banner */@charset \"utf-8\";"));
  CHECK(min->find("drop this comment") == std::string::npos);
  CHECK(min->find("a :first-child") != std::string::npos);              // descendant + pseudo-class keeps its space
  CHECK(min->find("calc(1px + 2px)") != std::string::npos);             // calc() operators keep their spaces
  CHECK(min->find("url( images/a b.png )") != std::string::npos);      // unquoted url() is verbatim
  CHECK(min->find("\"a ; b } c\"") != std::string::npos);               // strings are verbatim
  CHECK(min->find("(max-width:600px)") != std::string::npos);
  CHECK(min->find("color:red;margin:") != std::string::npos);
  CHECK(min->find(";}") == std::string::npos);                          // last semicolons dropped
  CHECK(*code::minify_css(*min) == *min);
  CHECK(*code::minify_css(*code::pretty_css(css)) == *min);
  const std::string pretty = *code::pretty_css("a{color:red;margin:0}b>c{x:y}");
  CHECK(pretty == "a {\n  color: red;\n  margin: 0;\n}\nb > c {\n  x: y;\n}\n");
  CHECK(convert("a { color : red ; }", "css", "css").text() == "a{color:red}");
  CHECK_STATUS(code::minify_css("a { content: \"x }"), Status::Decode);
}

TEST_CASE("code/html-min: whitespace-safe, raw text kept") {
  const std::string html = input("page.html");
  const test::Converted result = convert(html, "", "html-min", {}, "page.html");
  REQUIRE(result.ok());
  const std::string min = result.text();
  CHECK(min.find("<!-- a comment -->") == std::string::npos);
  CHECK(min.find("<!--[if IE]><p>old</p><![endif]-->") != std::string::npos);
  CHECK(min.find("<pre>  keep\n      this   </pre>") != std::string::npos);
  CHECK(min.find("<script>  if (a  <  b) { x = \"  keep  \" }  </script>") != std::string::npos);
  CHECK(min.find("<body class=\"main  page\">") != std::string::npos);
  CHECK(min.find("<p>Some <b>bold</b> text over lines.</p>") != std::string::npos);
  CHECK(min.size() < html.size());
  CHECK(convert(test::random_bytes(100, 1), "bin", "html-min").status == Status::Encode);
}

TEST_CASE("code/sql and code/graphql: minify and pretty") {
  const std::string sql = input("report.sql");
  const std::string min = *code::minify_sql(sql);
  CHECK(min.find("--") == min.find("-- not a comment"));                // only the one inside the string
  CHECK(min.find("/* inline */") == std::string::npos);
  CHECK(min.find('\n') == std::string::npos);
  const std::string pretty = *code::pretty_sql(sql);
  CHECK(pretty.find("\nFROM users u\nLEFT JOIN orders o ON") != std::string::npos);
  CHECK(pretty.find("\nGROUP BY u.id,\n  u.name\n") != std::string::npos);
  CHECK(pretty.find("COUNT(o.id)") != std::string::npos);
  CHECK(*code::pretty_sql("select a from t", "lower") == "select a\nfrom t\n");
  CHECK(convert(sql, "sql", "sql").text() == pretty);  // sql -> sql formats by default
  CHECK(*code::minify_sql("SELECT /*+ INDEX(t i) */ a FROM t -- x\n") == "SELECT /*+ INDEX(t i) */ a FROM t");

  const std::string graphql = input("hero.graphql");
  CHECK(*code::minify_graphql(graphql) ==
        "query Hero($episode:Episode=JEDI$withFriends:Boolean!){hero(episode:$episode){name...Details "
        "friends@include(if:$withFriends){name}}}fragment Details on Character{id appearsIn}");
  const std::string gpretty = *code::pretty_graphql(graphql);
  CHECK(gpretty.find("  hero(episode: $episode) {\n    name\n    ...Details\n") != std::string::npos);
  CHECK(*code::minify_graphql(gpretty) == *code::minify_graphql(graphql));
  CHECK_STATUS(code::minify_graphql("{ a(b: \"x) }"), Status::Decode);
}
