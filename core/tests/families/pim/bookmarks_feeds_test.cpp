// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Bookmarks (Netscape HTML, browser JSON), link files (.url, .webloc,
// .desktop) and news feeds (RSS, Atom, JSON Feed).
#include "engine/detect.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/table.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

std::string detected(std::string_view fixture_name, std::string_view filename) {
  const Bytes data = test::fixture(fixture_name);
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? detection->format->id : std::string("<nothing>");
}

}  // namespace

TEST_CASE("pim/netscape-bookmarks: decodes the browser export format") {
  const auto data = test::read_as<ir::Data>("netscape-bookmarks", test::fixture("pim/bookmarks/netscape.html"));
  REQUIRE_OK(data);
  const Json& root = data->value;
  CHECK(root["title"].as_string() == "Bookmarks");
  REQUIRE(root["children"].size() == 2);
  const Json& bar = root["children"][0];
  CHECK(bar["type"].as_string() == "folder");
  CHECK(bar["title"].as_string() == "Bookmarks bar");
  CHECK(bar["added"].as_string() == "2023-11-14T22:13:20Z");
  REQUIRE(bar["children"].size() == 2);
  CHECK(bar["children"][0]["title"].as_string() == "Example & Co");
  CHECK(bar["children"][0]["url"].as_string() == "https://www.example.org/");
  CHECK(bar["children"][0]["description"].as_string() == "A made-up site");
  CHECK(bar["children"][0]["tags"][1].as_string() == "docs");
  CHECK(bar["children"][1]["children"][0]["title"].as_string() == "שלום 🎉");
  CHECK(root["children"][1]["url"].as_string() == "https://example.com/other?a=1&b=2");

  const auto table = test::read_as<ir::Table>("netscape-bookmarks", test::fixture("pim/bookmarks/netscape.html"));
  REQUIRE_OK(table);
  const ir::Sheet& sheet = table->sheets[0];
  REQUIRE(sheet.rows.size() == 4);
  CHECK(sheet.rows[2][0].text == "Bookmarks bar/Ünïcödé folder");
  const auto document = test::read_as<ir::Document>("netscape-bookmarks", test::fixture("pim/bookmarks/netscape.html"));
  REQUIRE_OK(document);
  CHECK(ir::plain_text(*document).find("Ünïcödé folder") != std::string::npos);

  // html -> html keeps the tree; table -> html rebuilds the folders.
  const test::Converted again = test::convert(test::fixture("pim/bookmarks/netscape.html"), "netscape-bookmarks", "netscape-bookmarks");
  REQUIRE(again.ok());
  const auto copy = test::read_as<ir::Data>("netscape-bookmarks", again.data);
  REQUIRE_OK(copy);
  CHECK_MESSAGE(test::data_equal(root, copy->value), test::data_diff(root, copy->value));
  const auto from_table = test::write_from("netscape-bookmarks", *table);
  REQUIRE_OK(from_table);
  const auto rebuilt = test::read_as<ir::Table>("netscape-bookmarks", *from_table);
  REQUIRE_OK(rebuilt);
  CHECK_MESSAGE(test::tables_equal(*table, *rebuilt), test::table_diff(*table, *rebuilt));
}

TEST_CASE("pim/browser-bookmarks: Chrome and Firefox JSON") {
  const auto chrome = test::read_as<ir::Data>("browser-bookmarks", test::fixture("pim/bookmarks/chrome_bookmarks.json"));
  REQUIRE_OK(chrome);
  const Json& bar = chrome->value["children"][0];
  CHECK(bar["title"].as_string() == "Bookmarks bar");
  CHECK(bar["children"][0]["url"].as_string() == "https://www.example.org/");
  CHECK(bar["children"][0]["added"].as_string() == "2023-11-14T22:15:00Z");
  CHECK(bar["children"][1]["children"][0]["title"].as_string() == "שלום 🎉");
  const auto firefox = test::read_as<ir::Data>("browser-bookmarks", test::fixture("pim/bookmarks/firefox_backup.json"));
  REQUIRE_OK(firefox);
  const Json& menu = firefox->value["children"][0];
  REQUIRE(menu["children"].size() == 1);  // the separator is skipped
  CHECK(menu["children"][0]["tags"][0].as_string() == "example");
  // To the Netscape format every browser imports.
  const test::Converted html = test::convert(test::fixture("pim/bookmarks/chrome_bookmarks.json"), "browser-bookmarks",
                                             "netscape-bookmarks");
  REQUIRE(html.ok());
  CHECK(html.text().find("<DT><A HREF=\"https://www.example.org/\" ADD_DATE=\"1700000100\">Example &amp; Co</A>") !=
        std::string::npos);
}

TEST_CASE("pim/link files: .url, .webloc (XML and binary) and .desktop") {
  const auto url = test::read_as<ir::Data>("url", test::fixture("pim/bookmarks/example.url"));
  REQUIRE_OK(url);
  CHECK(url->value["url"].as_string() == "https://www.example.org/docs?x=1&y=2");
  const auto webloc = test::read_as<ir::Data>("webloc", test::fixture("pim/bookmarks/example.webloc"));
  REQUIRE_OK(webloc);
  CHECK(webloc->value["url"].as_string() == "https://www.example.org/docs?x=1&y=2");
  const auto binary = test::read_as<ir::Data>("webloc", test::fixture("pim/bookmarks/example_binary.webloc"));
  REQUIRE_OK(binary);
  CHECK(binary->value["url"].as_string() == "https://www.example.org/ünï");
  const auto desktop = test::read_as<ir::Data>("desktop", test::fixture("pim/bookmarks/example.desktop"));
  REQUIRE_OK(desktop);
  CHECK(desktop->value["url"].as_string() == "https://www.example.org/docs");
  CHECK(desktop->value["title"].as_string() == "Example docs");
  // Every pair of link formats.
  for (const char* from : {"url", "webloc", "desktop"}) {
    const auto written = test::write_from(from, *desktop);
    REQUIRE_OK(written);
    for (const char* to : {"url", "webloc", "desktop"}) {
      INFO(from, " -> ", to);
      const test::Converted converted = test::convert(*written, from, to);
      REQUIRE(converted.ok());
      const auto back = test::read_as<ir::Data>(to, converted.data);
      REQUIRE_OK(back);
      CHECK(back->value["url"].as_string() == "https://www.example.org/docs");
    }
  }
  CHECK_STATUS(test::read_as<ir::Data>("url", test::bytes("[InternetShortcut]\r\n")), Status::Decode);
}

TEST_CASE("pim/feeds: RSS, Atom and JSON Feed decode to one model") {
  const auto rss = test::read_as<ir::Data>("rss", test::fixture("pim/feeds/feedgen.rss"));
  const auto atom = test::read_as<ir::Data>("atom", test::fixture("pim/feeds/feedgen.atom"));
  const auto json = test::read_as<ir::Data>("json-feed", test::fixture("pim/feeds/feed.json"));
  REQUIRE_OK(rss);
  REQUIRE_OK(atom);
  REQUIRE_OK(json);
  for (const Json* feed : {&rss->value, &atom->value, &json->value}) {
    CHECK((*feed)["title"].as_string() == "Example News");
    CHECK((*feed)["link"].as_string() == "https://news.example.org/");
    CHECK((*feed)["language"].as_string() == "en");
    REQUIRE((*feed)["items"].size() == 2);
  }
  // RSS dates are converted from RFC 822; Atom and JSON Feed keep RFC 3339.
  CHECK(rss->value["items"][0]["published"].as_string() == "2024-05-01T12:30:00+02:00");
  CHECK(rss->value["items"][0]["summary"].as_string() == "Summary with <b>HTML</b>");
  CHECK(rss->value["items"][1]["enclosures"][0]["length"].as_int64() == 1234);
  CHECK(atom->value["items"][1]["title"].as_string() == "שלום from the newsroom 📰");
  CHECK(atom->value["items"][0]["published"].as_string() == "2024-05-01T12:30:00+02:00");
  CHECK(atom->value["author"].as_string() == "Ada Example");
  CHECK(json->value["items"][0]["enclosures"][0]["type"].as_string() == "audio/mpeg");
  CHECK(json->value["items"][0]["categories"][0].as_string() == "fixtures");
  const auto rdf = test::read_as<ir::Data>("rss", test::fixture("pim/feeds/rss1.rdf"));
  REQUIRE_OK(rdf);
  CHECK(rdf->value["items"][0]["author"].as_string() == "Ada Example");
  CHECK(rdf->value["items"][0]["published"].as_string() == "2024-05-01T12:30:00+02:00");
}

TEST_CASE("pim/feeds: every pair round-trips the items") {
  const auto source = test::read_as<ir::Data>("rss", test::fixture("pim/feeds/feedgen.rss"));
  REQUIRE_OK(source);
  for (const char* from : {"rss", "atom", "json-feed"}) {
    const auto written = test::write_from(from, *source);
    REQUIRE_OK(written);
    for (const char* to : {"rss", "atom", "json-feed"}) {
      INFO(from, " -> ", to);
      const test::Converted converted = test::convert(*written, from, to);
      REQUIRE(converted.ok());
      const auto back = test::read_as<ir::Data>(to, converted.data);
      REQUIRE_OK(back);
      REQUIRE(back->value["items"].size() == 2);
      for (std::size_t i = 0; i < 2; ++i) {
        const Json& a = source->value["items"][i];
        const Json& b = back->value["items"][i];
        for (const char* key : {"title", "link", "published", "summary"}) CHECK(a[key] == b[key]);
        CHECK(a["enclosures"] == b["enclosures"]);
        CHECK(a["categories"] == b["categories"]);
      }
      CHECK(back->value["title"].as_string() == "Example News");
    }
  }
  const auto document = test::read_as<ir::Document>("atom", test::fixture("pim/feeds/feedgen.atom"));
  REQUIRE_OK(document);
  CHECK(ir::plain_text(*document).find("Summary with HTML") != std::string::npos);
  const auto table = test::read_as<ir::Table>("json-feed", test::fixture("pim/feeds/feed.json"));
  REQUIRE_OK(table);
  CHECK(table->sheets[0].rows[1][2].type == ir::CellType::DateTime);
}

TEST_CASE("pim/bookmarks and feeds: detection and damaged input") {
  CHECK(detected("pim/bookmarks/netscape.html", "bookmarks.html") == "netscape-bookmarks");
  CHECK(detected("pim/bookmarks/chrome_bookmarks.json", "Bookmarks") == "browser-bookmarks");
  CHECK(detected("pim/bookmarks/firefox_backup.json", "backup.json") == "browser-bookmarks");
  CHECK(detected("pim/bookmarks/example.url", "") == "url");
  CHECK(detected("pim/bookmarks/example.webloc", "") == "webloc");
  CHECK(detected("pim/bookmarks/example_binary.webloc", "x.webloc") == "webloc");
  CHECK(detected("pim/bookmarks/example.desktop", "") == "desktop");
  CHECK(detected("pim/feeds/feedgen.rss", "feed.xml") == "rss");
  CHECK(detected("pim/feeds/rss1.rdf", "") == "rss");
  CHECK(detected("pim/feeds/feedgen.atom", "feed.xml") == "atom");
  CHECK(detected("pim/feeds/feed.json", "feed.json") == "json-feed");
  struct Case {
    const char* format;
    const char* file;
  };
  const Case cases[] = {{"netscape-bookmarks", "pim/bookmarks/netscape.html"},
                        {"browser-bookmarks", "pim/bookmarks/chrome_bookmarks.json"},
                        {"browser-bookmarks", "pim/bookmarks/firefox_backup.json"},
                        {"url", "pim/bookmarks/example.url"},
                        {"webloc", "pim/bookmarks/example.webloc"},
                        {"webloc", "pim/bookmarks/example_binary.webloc"},
                        {"desktop", "pim/bookmarks/example.desktop"},
                        {"rss", "pim/feeds/feedgen.rss"},
                        {"rss", "pim/feeds/rss1.rdf"},
                        {"atom", "pim/feeds/feedgen.atom"},
                        {"json-feed", "pim/feeds/feed.json"}};
  for (const Case& c : cases) {
    INFO(c.format, " ", c.file);
    CHECK(test::fuzz_reader<ir::Data>(c.format, test::fixture(c.file)) > 0);
  }
  // Deep folder nesting is refused, not recursed into.
  std::string deep = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n";
  for (int i = 0; i < 200; ++i) deep += "<DT><H3>f</H3><DL><p>";
  CHECK_STATUS(test::read_as<ir::Data>("netscape-bookmarks", test::bytes(deep)), Status::Limit);
}
