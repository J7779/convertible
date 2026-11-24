// SYLK, DIF and Lotus 1-2-3 / Quattro Pro.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <bit>
#include <string>

#include "engine/detect.hpp"
#include "families/table/table_test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using namespace cvt::table_test;

TEST_CASE("table/slk: decodes SYLK files written by Gnumeric and LibreOffice") {
  const auto gnumeric = read_fixture("slk", "gnumeric_sample.slk");
  REQUIRE_OK(gnumeric);
  ir::Sheet expected = types_sheet();
  expected.name = "";
  // SYLK is a code-page format: Gnumeric replaced what Windows-1252 cannot hold.
  expected.rows[2][8] = S("Gr??e, ??");
  expected.rows[5][8] = S("? smile");
  CHECK_MESSAGE(test::tables_equal(renamed(*gnumeric, ""), single(expected)), test::table_diff(renamed(*gnumeric, ""), single(expected)));
  // LibreOffice writes dates as plain serial numbers (no formats) and booleans as 1/0 with a formula.
  const auto lo = read_fixture("slk", "lo_sample.slk");
  REQUIRE_OK(lo);
  CHECK(lo->sheets[0].rows[1][5].type == ir::CellType::Number);
  CHECK(lo->sheets[0].rows[1][5].number == 45413);
  CHECK(lo->sheets[0].rows[4][8].text == "line one\nline two");
  CHECK(lo->sheets[0].formula_at(1, 4) == "TRUE()");
}

TEST_CASE("table/slk: write and read back") {
  ir::Table table = single(types_sheet());
  table.sheets[0].rows[4][8] = S("line one two");  // SYLK has no line breaks in values
  table.sheets[0].rows[2][8] = S("Grüße; semicolon");
  table.sheets[0].formulas[{1, 1}] = "A1*2";
  const auto bytes = test::write_from("slk", table, R"({"encoding":"utf-8"})");
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).starts_with("ID;P\n"));
  const auto back = test::read_as<ir::Table>("slk", *bytes);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(renamed(*back, "Types"), table), test::table_diff(renamed(*back, "Types"), table));
  CHECK(back->sheets[0].formula_at(1, 1) == "A1*2");
  CHECK(detect_format(Registry::instance(), *bytes, "")->format->id == "slk");
  CHECK(test::fuzz_reader<ir::Table>("slk", test::fixture("table/gnumeric_sample.slk")) > 0);
}

TEST_CASE("table/dif: decodes DIF files written by Gnumeric and LibreOffice") {
  const auto gnumeric = read_fixture("dif", "gnumeric_sample.dif");
  REQUIRE_OK(gnumeric);
  CHECK(gnumeric->sheets[0].rows[0][0].text == "Label");
  CHECK(gnumeric->sheets[0].rows[2][8].text == "Grüße, 世界");
  CHECK(gnumeric->sheets[0].rows[4][8].text == "line one\nline two");
  CHECK(gnumeric->sheets[0].rows[1][4].type == ir::CellType::Bool);
  CHECK(gnumeric->sheets[0].rows[1][5].number == 45413);  // DIF has no dates: serial numbers
  const auto lo = read_fixture("dif", "lo_sample.dif");
  REQUIRE_OK(lo);
  // LibreOffice puts formatted values into numeric items; they are recognised.
  CHECK(lo->sheets[0].rows[1][4].type == ir::CellType::Bool);
  CHECK(lo->sheets[0].rows[1][5].type == ir::CellType::DateTime);
  CHECK(lo->sheets[0].rows[1][5].text == "2024-05-01");
}

TEST_CASE("table/dif: write and read back") {
  const ir::Table table = single(make_sheet("", {{S("a"), S("b \"q\""), S("c")}, {N(1.5), B(true), E()}, {N(-2), B(false), S("x")}}));
  const auto bytes = test::write_from("dif", table);
  REQUIRE_OK(bytes);
  CHECK(test::str(*bytes).starts_with("TABLE\r\n0,1\r\n\"\"\r\nVECTORS\r\n0,3\r\n"));
  const auto back = test::read_as<ir::Table>("dif", *bytes);
  REQUIRE_OK(back);
  CHECK_MESSAGE(test::tables_equal(renamed(*back, ""), table), test::table_diff(renamed(*back, ""), table));
  CHECK(detect_format(Registry::instance(), *bytes, "")->format->id == "dif");
  CHECK(test::fuzz_reader<ir::Table>("dif", test::fixture("table/gnumeric_sample.dif")) > 0);
}

namespace {

// A WK1 worksheet assembled record by record from the Lotus file format
// description (no independent writer of the format was available).
Bytes lotus_sample(std::uint16_t version) {
  util::ByteWriter out;
  const auto record = [&](std::uint16_t type, const util::ByteWriter& body) {
    out.u16le(type).u16le(static_cast<std::uint16_t>(body.size())).bytes(body.view());
  };
  util::ByteWriter bof;
  bof.u16le(version);
  record(0x0000, bof);
  const auto cell = [](std::uint8_t format, std::uint16_t column, std::uint16_t row) {
    util::ByteWriter body;
    body.u8(format).u16le(column).u16le(row);
    return body;
  };
  util::ByteWriter label = cell(0xFF, 0, 0);
  label.text("'Name").u8(0);
  record(0x000F, label);
  util::ByteWriter label2 = cell(0xFF, 1, 0);
  label2.text("^When").u8(0);
  record(0x000F, label2);
  util::ByteWriter text = cell(0xFF, 0, 1);
  text.text("\"Caf\x82").u8(0);  // code page 437
  record(0x000F, text);
  util::ByteWriter date = cell(0x72, 1, 1);  // special format D1: a date
  date.f64le(45413);
  record(0x000E, date);
  util::ByteWriter integer = cell(0x02, 0, 2);
  integer.i16le(-7);
  record(0x000D, integer);
  util::ByteWriter formula = cell(0x02, 1, 2);
  formula.f64le(2.5).u16le(0);
  record(0x0010, formula);
  record(0x0001, util::ByteWriter{});
  return out.take();
}

}  // namespace

TEST_CASE("table/wk1: Lotus 1-2-3 and Quattro Pro worksheets") {
  const ir::Table expected = single(make_sheet("", {{S("Name"), S("When")}, {S("Café"), D("2024-05-01")}, {N(-7), N(2.5)}}));
  for (const auto& [format, version] : {std::pair<const char*, std::uint16_t>{"wk1", 0x0406}, {"wk1", 0x0404}, {"wq1", 0x5120}}) {
    INFO(format);
    const Bytes bytes = lotus_sample(version);
    const auto table = test::read_as<ir::Table>(format, bytes);
    REQUIRE_OK(table);
    CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), expected), test::table_diff(renamed(*table, ""), expected));
    CHECK(detect_format(Registry::instance(), bytes, "")->format->id == format);
    CHECK(test::fuzz_reader<ir::Table>(format, bytes) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Table>("wk1", lotus_sample(0x1000)), Status::Decode);  // WK3 and later
}
