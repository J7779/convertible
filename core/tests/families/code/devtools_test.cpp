// Tests of the developer interchange formats (diff.cpp, devtools.cpp).
// Diff fixtures were written by `diff -u` and `git diff` (tests/data/code/devtools/SOURCES.md);
// date, UUID and colour expectations were cross-checked with Python's
// datetime, email.utils, uuid and colorsys modules.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/code/devtools.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using test::convert;

namespace {
std::string detected(std::string_view data, std::string_view filename) {
  const auto detection = detect_format(Registry::instance(), as_bytes(data), filename);
  return detection ? std::string(detection->format->id) : std::string("none");
}
}  // namespace

TEST_CASE("code/diff: diff -u and git diff output") {
  const auto unified = code::parse_diff(test::fixture_text("code/devtools/sample.diff"));
  REQUIRE_OK(unified);
  REQUIRE(unified->size() == 1);
  CHECK((*unified)[0].old_path == "greek.txt");
  CHECK((*unified)[0].added == 2);
  CHECK((*unified)[0].removed == 1);
  REQUIRE((*unified)[0].hunks.size() == 1);
  CHECK((*unified)[0].hunks[0].lines.size() == 10);
  CHECK((*unified)[0].hunks[0].lines.back().new_number == 9);

  const auto git = code::parse_diff(test::fixture_text("code/devtools/sample.patch"));
  REQUIRE_OK(git);
  REQUIRE(git->size() == 3);
  CHECK((*git)[0].old_path == "/dev/null");
  CHECK((*git)[0].new_path == "added.txt");
  CHECK((*git)[1].new_path == "docs/numbers.txt");
  CHECK((*git)[1].added == 2);
  CHECK(detected(test::fixture_text("code/devtools/sample.patch"), "change.patch") == "diff");
  CHECK(detected(test::fixture_text("code/devtools/sample.diff"), "") == "diff");

  const auto inline_doc = test::read_as<ir::Document>("diff", test::fixture("code/devtools/sample.patch"));
  REQUIRE_OK(inline_doc);
  const std::string text = ir::plain_text(*inline_doc);
  CHECK(text.starts_with("3 files changed, 5 insertions(+), 2 deletions(-)"));
  CHECK(text.find("added.txt (new file)") != std::string::npos);
  std::size_t code_blocks = 0;
  for (const ir::Block& block : inline_doc->blocks) code_blocks += block.is<ir::CodeBlock>() ? 1 : 0;
  CHECK(code_blocks == 3);

  const auto side = test::read_as<ir::Document>("diff", test::fixture("code/devtools/sample.diff"), R"({"layout":"side-by-side"})");
  REQUIRE_OK(side);
  const ir::TableBlock* table = nullptr;
  for (const ir::Block& block : side->blocks) {
    if (const auto* t = block.get_if<ir::TableBlock>()) table = t;
  }
  REQUIRE(table != nullptr);
  // header + hunk header + 9 rows (beta/BETA paired, iota alone)
  CHECK(table->rows.size() == 11);
  CHECK(ir::plain_text(table->rows[2].cells[1].content) == "alpha");
  CHECK(ir::plain_text(table->rows[3].cells[1].content) == "beta");
  CHECK(ir::plain_text(table->rows[3].cells[3].content) == "BETA");
  // Through the engine: highlighted HTML of a patch.
  const std::string html = convert(test::fixture("code/devtools/sample.patch"), "diff", "html-highlight").text();
  CHECK(html.find("hl-inserted") != std::string::npos);
  CHECK_STATUS(code::parse_diff("just text\n"), Status::Decode);
  CHECK(test::fuzz_reader<ir::Document>("diff", test::fixture("code/devtools/sample.patch")) > 0);
}

TEST_CASE("code/gitignore, editorconfig, npmrc: Data round trips") {
  const std::string ignore = "# build\n/build/\n*.o\n!keep.o\ndocs/*.tmp\n";
  const auto data = test::read_as<ir::Data>("gitignore", as_bytes(ignore));
  REQUIRE_OK(data);
  const Json& patterns = data->value["patterns"];
  REQUIRE(patterns.size() == 4);
  CHECK(patterns[0]["directory_only"].as_bool());
  CHECK(patterns[0]["anchored"].as_bool());
  CHECK(patterns[2]["negated"].as_bool());
  CHECK(patterns[3]["anchored"].as_bool());
  CHECK(!patterns[1]["anchored"].as_bool());
  CHECK(test::str(*test::write_from("gitignore", ir::Data{data->value})) == "/build/\n*.o\n!keep.o\ndocs/*.tmp\n");
  CHECK(detected(ignore, ".gitignore") == "gitignore");
  CHECK(detected("node_modules\n", ".dockerignore") == "dockerignore");

  const std::string editorconfig = "root = true\n\n[*]\nindent_style = space\nindent_size = 2\n\n[Makefile]\nindent_style = tab\n";
  const auto ec = test::read_as<ir::Data>("editorconfig", as_bytes(editorconfig));
  REQUIRE_OK(ec);
  CHECK(ec->value["root"].as_bool());
  CHECK(ec->value["sections"]["*"]["indent_size"].as_int64() == 2);
  CHECK(test::str(*test::write_from("editorconfig", ir::Data{ec->value})) == editorconfig);
  CHECK(detected(editorconfig, ".editorconfig") == "editorconfig");

  const std::string npmrc = "registry=https://registry.example.invalid/\nstrict-ssl=false\nca[]=one\nca[]=two\n";
  const auto rc = test::read_as<ir::Data>("npmrc", as_bytes(npmrc));
  REQUIRE_OK(rc);
  CHECK(!rc->value["strict-ssl"].as_bool(true));
  CHECK(rc->value["ca"].size() == 2);
  CHECK(test::str(*test::write_from("npmrc", ir::Data{rc->value})) == npmrc);
}

TEST_CASE("code/curl: commands, raw HTTP and code snippets") {
  const auto request = code::parse_curl(
      "curl -X PUT 'https://api.example.invalid/v1/items/7?dry=1' \\\n  -H 'Content-Type: application/json' "
      "-H \"X-Token: a b\" --data-raw '{\"name\":\"it'\\''s\"}' -u user:secret -sSL --compressed");
  REQUIRE_OK(request);
  CHECK(request->find("method")->as_string() == "PUT");
  CHECK(request->find("url")->as_string() == "https://api.example.invalid/v1/items/7?dry=1");
  CHECK((*request)["headers"]["X-Token"].as_string() == "a b");
  CHECK((*request)["body"].as_string() == "{\"name\":\"it's\"}");
  CHECK((*request)["auth"]["password"].as_string() == "secret");
  CHECK((*request)["options"]["follow_redirects"].as_bool());
  // Data posted with -d and no method is a form POST; -G moves it into the query.
  const auto form = code::parse_curl("curl example.invalid/search -d q=x -d page=2");
  REQUIRE_OK(form);
  CHECK((*form)["method"].as_string() == "POST");
  CHECK((*form)["headers"]["Content-Type"].as_string() == "application/x-www-form-urlencoded");
  const auto get = code::parse_curl("curl -G example.invalid/search --data-urlencode 'q=a b&c'");
  REQUIRE_OK(get);
  CHECK((*get)["url"].as_string() == "http://example.invalid/search?q=a%20b%26c");
  // Our own curl output parses back to the same request.
  const auto again = code::parse_curl(*code::to_curl(*request));
  REQUIRE_OK(again);
  CHECK(util::dump_json(*again) == util::dump_json(*request));
  // Raw HTTP both ways.
  const std::string http = *code::to_http_request(*request);
  CHECK(http.starts_with("PUT /v1/items/7?dry=1 HTTP/1.1\r\nHost: api.example.invalid\r\n"));
  CHECK(http.find("Authorization: Basic dXNlcjpzZWNyZXQ=\r\n") != std::string::npos);
  const auto parsed = code::parse_http_request(http);
  REQUIRE_OK(parsed);
  CHECK((*parsed)["url"].as_string() == "https://api.example.invalid/v1/items/7?dry=1");
  CHECK((*parsed)["body"].as_string() == "{\"name\":\"it's\"}");
  // Snippets through the engine (cURL -> Data -> code).
  const std::string curl = "curl -H 'Accept: text/plain' https://example.invalid/x";
  CHECK(convert(curl, "curl", "python-requests").text().find("requests.get(\n    \"https://example.invalid/x\"") !=
        std::string::npos);
  CHECK(convert(curl, "curl", "fetch-js").text().starts_with("const response = await fetch(\"https://example.invalid/x\""));
  CHECK(detected(curl, "request.txt") == "curl");
  CHECK_STATUS(code::parse_curl("wget https://x"), Status::Decode);
  CHECK_STATUS(code::parse_curl("curl 'unterminated"), Status::Decode);
}

TEST_CASE("code/cron: English descriptions") {
  CHECK(*code::describe_cron("30 5 * * 1-5") == "At 05:30 on Monday through Friday.");
  CHECK(*code::describe_cron("*/15 * * * *") == "Every 15 minutes.");
  CHECK(*code::describe_cron("0 0 1 JAN *") == "At 00:00 on day-of-month 1 in January.");
  CHECK(*code::describe_cron("0 9,17 * * MON,WED,FRI") == "At 09:00 and 17:00 on Monday, Wednesday and Friday.");
  CHECK(*code::describe_cron("@weekly") == "At 00:00 on Sunday.");
  CHECK(*code::describe_cron("0 12 * * 7") == "At 12:00 on Sunday.");
  CHECK(*code::describe_cron("5 * * * *") == "At minute 5 past every hour.");
  CHECK(*code::describe_cron("0 0 13 * 5") == "At 00:00 on day-of-month 13 or Friday.");
  CHECK_STATUS(code::describe_cron("61 * * * *"), Status::Decode);
  CHECK_STATUS(code::describe_cron("* * *"), Status::Decode);
  CHECK_STATUS(code::describe_cron("0 0 L * *"), Status::Decode);
  const auto data = code::cron_to_data("0 */6 * * *");
  REQUIRE_OK(data);
  CHECK((*data)["hour"].size() == 4);
  const std::string crontab = "SHELL=/bin/sh\n# nightly\n30 2 * * * /usr/local/bin/backup --all\n@reboot /usr/bin/start\n";
  const std::string text = convert(crontab, "cron", "txt").text();
  CHECK(text.find("30 2 * * *  →  At 02:30.  (/usr/local/bin/backup --all)") != std::string::npos);
  CHECK(text.find("@reboot  →  At system startup.") != std::string::npos);
  CHECK(detected(crontab, "crontab") == "cron");
}

TEST_CASE("code/datetime: Unix, ISO 8601 and RFC 2822 conversions") {
  const auto unix_time = code::parse_datetime("1700000000");
  REQUIRE_OK(unix_time);
  CHECK(code::format_datetime(*unix_time, "rfc2822") == "Tue, 14 Nov 2023 22:13:20 +0000");  // email.utils.format_datetime
  CHECK(code::format_datetime(*code::parse_datetime("1700000000123"), "iso8601") == "2023-11-14T22:13:20.123Z");
  const auto iso = code::parse_datetime("2024-02-29T12:34:56.789+02:00");
  REQUIRE_OK(iso);
  CHECK(code::format_datetime(*iso, "unix_ms") == "1709202896789");  // datetime.fromisoformat(...).timestamp()
  CHECK(code::format_datetime(*iso, "iso8601") == "2024-02-29T12:34:56.789+02:00");
  CHECK(code::format_datetime(*code::parse_datetime("Tue, 1 Jul 2003 10:52:37 +0200"), "unix") == "1057049557");
  CHECK(code::format_datetime(*code::parse_datetime("-1"), "iso8601") == "1969-12-31T23:59:59Z");
  CHECK(code::format_datetime(*code::parse_datetime("20240101T000000Z"), "unix") == "1704067200");
  CHECK(code::format_datetime(*code::parse_datetime("1 Jan 99 00:00 GMT"), "iso8601") == "1999-01-01T00:00:00Z");
  CHECK_STATUS(code::parse_datetime("2023-02-29"), Status::Decode);
  CHECK_STATUS(code::parse_datetime("99999999999999999999"), Status::Decode);
  CHECK(convert("1700000000\n\n2024-02-29T12:34:56+02:00\n", "txt", "datetime", R"({"timezone":"utc"})").text() ==
        "2023-11-14T22:13:20Z\n\n2024-02-29T10:34:56Z\n");
  CHECK(convert("not a date\n", "txt", "datetime").status == Status::Decode);
}

TEST_CASE("code/uuid-info and code/color") {
  const std::string v1 = *code::describe_identifier("c232ab00-9414-11ec-b3c8-9f6bdeced846");
  CHECK(v1.find("Version: 1") != std::string::npos);
  CHECK(v1.find("Time: 2022-02-22T19:22:22Z") != std::string::npos);  // uuid.UUID(...).time
  CHECK(v1.find("Clock sequence: 13256") != std::string::npos);
  CHECK(code::describe_identifier("{017F22E2-79B0-7CC3-98C4-DC0C0C07398F}")->find("Time: 2022-02-22T19:22:22Z") != std::string::npos);
  CHECK(code::describe_identifier("01ARZ3NDEKTSV4RRFFQ69G5FAV")->find("Time: 2016-07-30T23:54:10.259Z") != std::string::npos);
  CHECK(code::describe_identifier("00000000-0000-0000-0000-000000000000")->find("nil UUID") != std::string::npos);
  CHECK_STATUS(code::describe_identifier("1234"), Status::Decode);

  CHECK(code::format_color(*code::parse_color("red"), "oklch") == "oklch(62.8% 0.2577 29.23)");  // CSS Color 4 example
  CHECK(code::format_color(*code::parse_color("hsl(120 100% 25%)"), "hex") == "#008000");          // colorsys.hls_to_rgb
  CHECK(code::format_color(*code::parse_color("#0f08"), "rgb") == "rgb(0 255 0 / 53.3%)");
  CHECK(code::format_color(*code::parse_color("rgb(255, 165, 0)"), "name") == "orange");
  CHECK(code::format_color(*code::parse_color("oklch(62.8% 0.2577 29.23)"), "hex") == "#ff0000");
  CHECK(code::format_color(*code::parse_color("hwb(0 0% 0%)"), "hex") == "#ff0000");
  CHECK(code::format_color(*code::parse_color("rebeccapurple"), "hsl") == "hsl(270 50% 40%)");
  CHECK_STATUS(code::parse_color("nope"), Status::Decode);
  CHECK(convert("#ffffff\n", "txt", "color", R"({"to":"oklch"})").text() == "oklch(100% 0 0)\n");
}
