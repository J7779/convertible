// Bridges (plain text, table, JSON syntax tree), document utilities and notebooks.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/document/doc_test_support.hpp"

using namespace cvt;
using namespace cvt::test::doc;

TEST_CASE("document/plain text: layout of a document") {
  document::PlainTextOptions options;
  options.wrap_width = 40;
  const std::string text = document::to_plain_text(rich_document(), options);
  CHECK(text.starts_with("Heading One\n===========\n\n"));
  CHECK(text.find("Lists\n-----") != std::string::npos);
  CHECK(text.find("* First item\n* Second item\n  * Nested one\n  * Nested two\n* Third item\n") != std::string::npos);
  CHECK(text.find("1. Step one\n2. Step two\n3. Step three\n") != std::string::npos);
  CHECK(text.find("+--------+-----+-------+\n| Name   | Qty | Price |\n+========+=====+=======+\n") != std::string::npos);
  CHECK(text.find("|  1.25 |") == std::string::npos);  // center-aligned column
  CHECK(text.find("    def f(x):\n        return x < 1") != std::string::npos);
  CHECK(text.find("> Quoted text here.") != std::string::npos);
  CHECK(text.find("a link [1]") != std::string::npos);  // notes and links share one numbering
  CHECK(text.find("A note[2] here.") != std::string::npos);
  CHECK(text.find("[2] The note body.") != std::string::npos);
  CHECK(text.find("[1] https://example.com/a?b=1&c=2") != std::string::npos);
  for (const std::string_view line : util::split_lines(text)) {
    if (line.find("https://") == std::string_view::npos && !line.starts_with("|") && !line.starts_with("+") &&
        !line.starts_with("    ")) {
      CHECK(document::display_width(line) <= 40);
    }
  }
  // Wide characters count twice.
  CHECK(document::display_width("汉字ab🙂") == 8);
  CHECK(document::display_width("e\xCC\x81") == 1);
  CHECK(document::wrap_text("aa bb cc dd", 5, "> ", "  ") == "> aa\n  bb\n  cc\n  dd");
}

TEST_CASE("document/plain text: bridges through the engine") {
  const test::Converted text = test::convert(test::fixture("document/pandoc/sample.rst"), "rst", "txt", R"({"wrap_width":60})");
  REQUIRE(text.ok());
  CHECK(text.text().find("Lorem Ipsum Overview\n====================") != std::string::npos);
  const test::Converted inline_links = test::convert(test::fixture("document/pandoc/sample.rst"), "rst", "txt",
                                                     R"({"link_references":false,"ascii_tables":false})");
  REQUIRE(inline_links.ok());
  CHECK(test::normalized_text(inline_links.text()).find("example site <https://example.com/docs>") != std::string::npos);
  CHECK(inline_links.text().find("Apple\t3\t1.25") != std::string::npos);
  // Plain text in: structure is recognised (or not, on request).
  const std::string plain =
      "TITLE IN CAPS\n\nIntro paragraph\nwrapped over lines.\n\n    code line\n      indented\n\nSection\n-------\n\n- one\n- two\n  continued\n\n"
      "1. first\n2. second\n\n> quoted\n\n2.1 Numbered Heading\n\nEnd.\n";
  const auto doc = document::infer_document_from_text(plain);
  CHECK(outline(doc) == std::vector<std::string>{"1:TITLE IN CAPS", "2:Section", "2:2.1 Numbered Heading"});
  const auto all = lists(doc);
  REQUIRE(all.size() == 2);
  CHECK(!all[0]->ordered);
  CHECK(ir::plain_text(all[0]->items[1]) == "two\ncontinued\n");
  CHECK(all[1]->ordered);
  REQUIRE(code_blocks(doc).size() == 1);
  CHECK(code_blocks(doc)[0]->code == "code line\n  indented");
  CHECK(count_blocks(doc, [](const ir::Block& b) { return b.is<ir::BlockQuote>(); }) == 1);
  const test::Converted html = test::convert(plain, "txt", "html");
  REQUIRE(html.ok());
  CHECK(html.text().find("<h2>Section</h2>") != std::string::npos);
  const test::Converted flat = test::convert(plain, "txt", "html", R"({"infer_structure":false})");
  REQUIRE(flat.ok());
  CHECK(flat.text().find("<h2>") == std::string::npos);
  CHECK(flat.text().find("<ul>") == std::string::npos);
}

TEST_CASE("document/bridges: tables are extracted as sheets") {
  const test::Converted csv_like = test::convert(test::fixture("document/pandoc/sample.html"), "html", "md");
  REQUIRE(csv_like.ok());
  // Document -> Table through a registered bridge.
  const Bridge* bridge = nullptr;
  for (const Bridge& b : Registry::instance().bridges()) {
    if (b.from == Kind::Document && b.to == Kind::Table) bridge = &b;
  }
  REQUIRE(bridge != nullptr);
  ir::Document d = sample();
  d.blocks.push_back(ir::build::table({ir::build::row({"x", "y"}, true), ir::build::row({"1", "two"})}));
  Context ctx;
  auto table = bridge->fn(AnyIR(d), ctx);
  REQUIRE_OK(table);
  const ir::Table& t = table->get<ir::Table>();
  REQUIRE(t.sheets.size() == 2);
  CHECK(t.sheets[0].has_header);
  CHECK(t.sheets[0].rows.size() == 4);
  CHECK(t.sheets[0].rows[1][1].type == ir::CellType::Number);
  CHECK(t.sheets[0].rows[1][1].number == 3.0);
  CHECK(t.sheets[0].rows[3][2].text == "9.99");
  ctx.options = Options(Json::object({{"tables", "first"}}));
  auto first = bridge->fn(AnyIR(d), ctx);
  REQUIRE_OK(first);
  CHECK(first->get<ir::Table>().sheets.size() == 1);
  ir::Document none;
  none.blocks.push_back(ir::build::paragraph("no tables"));
  CHECK_STATUS(bridge->fn(AnyIR(none), ctx), Status::Encode);
}

TEST_CASE("document/bridges: the JSON syntax tree round-trips exactly") {
  const Bridge* to_data = nullptr;
  const Bridge* from_data = nullptr;
  for (const Bridge& b : Registry::instance().bridges()) {
    if (b.from == Kind::Document && b.to == Kind::Data) to_data = &b;
    if (b.from == Kind::Data && b.to == Kind::Document) from_data = &b;
  }
  REQUIRE(to_data != nullptr);
  REQUIRE(from_data != nullptr);
  ir::Document d = rich_document();
  ir::EncodedImage png;
  png.bytes = test::fixture("document/sample/pixel.png");
  png.mime = "image/png";
  d.blocks.push_back(ir::build::image_block(png, "Pixel", "Caption"));
  d.blocks.back().attr.id = "fig";
  d.blocks.front().attr.classes = {"a", "b"};
  Context ctx;
  auto data = to_data->fn(AnyIR(d), ctx);
  REQUIRE_OK(data);
  const Json json = data->get<ir::Data>().value;
  CHECK(json["type"].as_string() == "document");
  auto back = from_data->fn(AnyIR(ir::Data{json}), ctx);
  REQUIRE_OK(back);
  auto again = to_data->fn(AnyIR(back->get<ir::Document>()), ctx);
  REQUIRE_OK(again);
  CHECK(test::data_diff(json, again->get<ir::Data>().value).empty());
  // Other data is laid out readably.
  Json other = Json::object({{"name", "x"}, {"rows", Json::array({Json::object({{"a", 1}}), Json::object({{"a", 2}})})}});
  auto laid_out = from_data->fn(AnyIR(ir::Data{other}), ctx);
  REQUIRE_OK(laid_out);
  CHECK(first_table(laid_out->get<ir::Document>()) != nullptr);
}

TEST_CASE("document/utilities: slugs, outline, contents, notes, tables, depth") {
  CHECK(document::slugify("Hello, World! Ça va?") == "hello-world-ça-va");
  std::set<std::string> used;
  CHECK(document::unique_slug("Intro", used) == "intro");
  CHECK(document::unique_slug("Intro", used) == "intro-1");
  ir::Document d = rich_document();
  document::insert_table_of_contents(d, 2);
  REQUIRE(d.blocks.size() > 2);
  CHECK(d.blocks[1].get_if<ir::Heading>() != nullptr);
  CHECK(d.blocks[1].attr.has_class("toc-title"));
  const auto* toc = d.blocks[2].get_if<ir::List>();
  REQUIRE(toc != nullptr);
  const auto* entry = toc->items.front().front().get_if<ir::Paragraph>();
  REQUIRE(entry != nullptr);
  const auto* link = entry->content.front().get_if<ir::Link>();
  REQUIRE(link != nullptr);
  CHECK(link->url == "#heading-one");
  CHECK(ir::plain_text(link->children) == "Heading One");
  // Footnote renumbering by first reference; unreferenced notes dropped.
  ir::Document notes;
  notes.blocks.push_back(ir::build::paragraph({ir::Link{"#fn-b", {}, {ir::Text{"x"}}, "b"}, ir::Link{"#fn-a", {}, {ir::Text{"y"}}, "a"}}));
  notes.footnotes = {{"a", {ir::build::paragraph("A")}}, {"b", {ir::build::paragraph("B")}}, {"c", {ir::build::paragraph("C")}}};
  document::number_footnotes(notes);
  REQUIRE(notes.footnotes.size() == 2);
  CHECK(notes.footnotes[0].label == "1");
  CHECK(ir::plain_text(notes.footnotes[0].blocks) == "B\n");
  document::footnotes_to_endnotes(notes);
  CHECK(notes.footnotes.empty());
  CHECK(notes.blocks.back().attr.has_class("footnotes"));
  // Spans resolved into a grid.
  ir::TableBlock t;
  t.rows.push_back({{{{ir::Text{"A"}}, 2, 1}, {{ir::Text{"B"}}, 1, 2}}, true});
  t.rows.push_back({{{{ir::Text{"C"}}, 1, 1}, {{ir::Text{"D"}}, 1, 1}}, false});
  const document::TableGrid grid = document::table_grid(t);
  CHECK(grid.columns == 3);
  CHECK(grid.cells[1][2] == &t.rows[0].cells[1]);
  CHECK(!grid.origin[1][2]);
  const ir::TableBlock flat = document::normalize_table(t);
  CHECK(cell_text(flat, 1, 0) == "C");
  CHECK(cell_text(flat, 1, 1) == "D");
  CHECK(cell_text(flat, 1, 2) == "");
  CHECK(cell_text(document::normalize_table(t, true), 1, 2) == "B");
  // Depth enforcement flattens without recursion trouble.
  ir::Document deep;
  ir::Blocks* level = &deep.blocks;
  for (int i = 0; i < 200; ++i) {
    level->push_back(ir::BlockQuote{});
    level = &level->back().get_if<ir::BlockQuote>()->blocks;
  }
  level->push_back(ir::build::paragraph("bottom"));
  document::enforce_depth(deep);
  int max_depth = 0;
  walk_blocks(deep.blocks, [&](const ir::Block&, int depth) { max_depth = std::max(max_depth, depth); });
  CHECK(max_depth < ir::max_document_depth);
  CHECK(ir::plain_text(deep).find("bottom") != std::string::npos);
  // Shift and strip.
  ir::Document meta = rich_document();
  meta.metadata.author = "X";
  meta.metadata.extra["k"] = "v";
  document::strip_metadata(meta);
  CHECK(meta.metadata.author.empty());
  CHECK(meta.metadata.extra.empty());
  CHECK(!meta.metadata.title.empty());
  document::shift_headings(meta, 7);
  CHECK(document::heading_outline(meta).front().level == 6);
  // Data URIs and images.
  const auto image = document::parse_data_uri_image("data:image/png;base64,iVBORw0KGgo%3D");
  REQUIRE(image.has_value());
  CHECK(image->mime == "image/png");
  ir::Image pixels;
  pixels.frames.push_back(test::gradient_frame(3, 2));
  const auto encoded = document::encoded_image(pixels);
  REQUIRE(encoded.has_value());
  CHECK(encoded->mime == "image/png");
  CHECK(document::sniff_image_mime(encoded->bytes) == "image/png");
}

TEST_CASE("document/ipynb: notebooks with outputs, and writing notebooks") {
  const auto nb = test::read_as<ir::Document>("ipynb", test::fixture("document/python/outputs.ipynb"));
  REQUIRE_OK(nb);
  CHECK(heading_texts(*nb) == std::vector<std::string>{"Analysis"});
  const auto codes = code_blocks(*nb);
  REQUIRE(codes.size() >= 7);
  CHECK(codes[0]->code == "print(\"hello\")");
  CHECK(codes[0]->language == "python");
  CHECK(codes[1]->code == "hello");
  CHECK(codes[3]->code == "42");
  bool image = false;
  test::doc::walk_blocks(nb->blocks, [&](const ir::Block& b, int) {
    if (const auto* img = b.get_if<ir::ImageBlock>()) {
      image = std::get<ir::EncodedImage>(img->source).bytes == test::fixture("document/sample/pixel.png");
    }
  });
  CHECK(image);
  CHECK(codes.back()->code == "ZeroDivisionError: division by zero");  // ANSI colours stripped
  // Pandoc's notebook: the picture is a cell attachment.
  const auto pandoc_nb = test::read_as<ir::Document>("ipynb", test::fixture("document/pandoc/sample.ipynb"));
  REQUIRE_OK(pandoc_nb);
  bool attachment = false;
  test::doc::walk_blocks(pandoc_nb->blocks, [&](const ir::Block& b, int) {
    if (const auto* img = b.get_if<ir::ImageBlock>()) attachment = !std::get<ir::EncodedImage>(img->source).bytes.empty();
  });
  CHECK(attachment);
  // Write: code cells with their outputs.
  const auto written = test::write_from("ipynb", *nb);
  REQUIRE_OK(written);
  auto json = util::parse_json(test::str(*written));
  REQUIRE_OK(json);
  CHECK((*json)["nbformat"].as_int64() == 4);
  int code_cells = 0;
  int outputs = 0;
  for (const Json& cell : (*json)["cells"].elements()) {
    if (cell["cell_type"].as_string() == "code") {
      ++code_cells;
      outputs += static_cast<int>(cell["outputs"].size());
    }
  }
  CHECK(code_cells == 4);
  CHECK(outputs == 4);
  const auto back = test::read_as<ir::Document>("ipynb", *written);
  REQUIRE_OK(back);
  CHECK(test::text_similarity(*back, *nb) >= 0.98);
  CHECK(test::convert(*written, "ipynb", "ipynb").ok());
}
