// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>

#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::cell;
using test::science::detected;

namespace {

double num(const ir::Table& t, std::size_t row, std::size_t col, std::size_t sheet = 0) {
  return t.sheets[sheet].rows[row][col].number;
}

}  // namespace

TEST_CASE("science/jcamp: decodes an AFFN file written by the jcamp package") {
  const auto table = test::read_as<ir::Table>("jcamp", test::fixture("science/chem/band_affn.jdx"));
  REQUIRE_OK(table);
  const auto& rows = table->sheets[0].rows;
  REQUIRE(rows.size() == 26);
  CHECK(cell(*table, 0, 0) == "x (1/cm)");
  for (std::size_t i = 0; i < 25; ++i) {
    const double x = 400.0 + 50.0 * static_cast<double>(i);
    const double y = std::round(1e6 * std::exp(-std::pow((x - 1000.0) / 200.0, 2))) / 1e4;
    CHECK_APPROX(num(*table, i + 1, 0), x, 1e-9);
    CHECK_APPROX(num(*table, i + 1, 1), y, 1e-9);
  }
  CHECK(table->sheets.back().name == "header");
  CHECK(detected(test::fixture("science/chem/band_affn.jdx")) == "jcamp");
}

TEST_CASE("science/jcamp: decodes SQZ/DIF/DUP with Y check values") {
  const auto table = test::read_as<ir::Table>("jcamp", test::fixture("science/chem/asdf_example.jdx"));
  REQUIRE_OK(table);
  const std::vector<double> expected = {5, 10, 15, 15, 15, 12.5, 10, 10, 10, 10, 7.5, -2.5, -2.5, 0};
  REQUIRE(table->sheets[0].rows.size() == expected.size() + 1);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK_APPROX(num(*table, i + 1, 0), static_cast<double>(i), 1e-12);
    CHECK_APPROX(num(*table, i + 1, 1), expected[i], 1e-12);
  }
}

TEST_CASE("science/jcamp: ASDF and AFFN writers round trip; ASDF is smaller") {
  ir::Table table;
  ir::Sheet sheet;
  sheet.rows.push_back({ir::Cell::string("x"), ir::Cell::string("y")});
  for (int i = 0; i < 500; ++i) {
    const double x = 1000.0 + 2.0 * i;
    const double y = 50.0 + 40.0 * std::sin(i / 20.0) + (i % 50 == 0 ? 0.0 : 0.0);
    sheet.rows.push_back({ir::Cell::numeric(x, ""), ir::Cell::numeric(std::round(y * 1e4) / 1e4, "")});
  }
  sheet.has_header = true;
  table.sheets.push_back(sheet);
  const auto asdf = test::write_from("jcamp", table);
  const auto affn = test::write_from("jcamp", table, R"({"jcamp_form":"affn"})");
  REQUIRE_OK(asdf);
  REQUIRE_OK(affn);
  CHECK(asdf->size() < affn->size());
  for (const Bytes* bytes : {&*asdf, &*affn}) {
    const auto back = test::read_as<ir::Table>("jcamp", *bytes);
    REQUIRE_OK(back);
    REQUIRE(back->sheets[0].rows.size() == 501);
    for (std::size_t i = 1; i <= 500; ++i) {
      CHECK_APPROX(num(*back, i, 0), num(table, i, 0), 1e-9);
      CHECK_APPROX(num(*back, i, 1), num(table, i, 1), 1e-6);
    }
  }
  // The jcamp fixture survives jcamp -> jcamp.
  const auto fixture = test::read_as<ir::Table>("jcamp", test::fixture("science/chem/band_affn.jdx"));
  REQUIRE_OK(fixture);
  const auto again = test::write_from("jcamp", *fixture);
  REQUIRE_OK(again);
  CHECK(test::str(*again).find("##TITLE=synthetic band\n") != std::string::npos);
  const auto back = test::read_as<ir::Table>("jcamp", *again);
  REQUIRE_OK(back);
  CHECK(test::tables_equal(ir::Table{{back->sheets[0]}}, ir::Table{{fixture->sheets[0]}}, 1e-7));
  // Unevenly spaced data uses XYPOINTS.
  ir::Table uneven = table;
  uneven.sheets[0].rows[3][0].number = 1004.5;
  const auto points = test::write_from("jcamp", uneven);
  REQUIRE_OK(points);
  CHECK(test::str(*points).find("##XYPOINTS=(XY..XY)") != std::string::npos);
}

TEST_CASE("science/mzml: decodes an OpenMS file (zlib, 64-bit m/z, 32-bit intensity)") {
  for (const char* format : {"mzml", "mzxml"}) {
    INFO(format);
    const auto table = test::read_as<ir::Table>(
        format, test::fixture(std::string("science/chem/") + (std::string_view(format) == "mzml" ? "run.mzML" : "run.mzXML")));
    REQUIRE_OK(table);
    const auto& peaks = table->sheets[0].rows;
    REQUIRE(peaks.size() == 7);
    const double mz[] = {100.5, 200.25, 300.125, 400.0625, 150.0, 151.5};
    const double intensity[] = {1000.0, 2500.5, 50.25, 7.0, 10.0, 20.0};
    for (std::size_t i = 0; i < 6; ++i) {
      CHECK(num(*table, i + 1, 3) == mz[i]);
      CHECK(num(*table, i + 1, 4) == intensity[i]);
    }
    CHECK(num(*table, 1, 1) == 1);
    CHECK(num(*table, 5, 1) == 2);
    CHECK(num(*table, 1, 2) == 90.0);
    CHECK(num(*table, 6, 2) == 105.0);
    CHECK(table->sheets[1].rows.size() == 3);
  }
  CHECK(detected(test::fixture("science/chem/run.mzML")) == "mzml");
  CHECK(detected(test::fixture("science/chem/run.mzXML")) == "mzxml");
}

TEST_CASE("science/spectra: hostile input") {
  CHECK_STATUS(test::read_as<ir::Table>("jcamp", as_bytes("##TITLE=x\n##XYDATA=(X++(Y..Y))\n1 A0Z999999999\n##END=\n")),
               Status::Limit);
  CHECK_STATUS(test::read_as<ir::Table>("jcamp", as_bytes("##TITLE=x\n##END=\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Table>("mzml", as_bytes("<mzML><spectrum><binaryDataArray><cvParam accession=\"MS:1000514\"/>"
                                                          "<cvParam accession=\"MS:1000574\"/><binary>AAAA</binary>"
                                                          "</binaryDataArray></spectrum></mzML>")),
               Status::Decode);
  for (const auto& [format, name] : std::vector<std::pair<std::string, std::string>>{
           {"jcamp", "band_affn.jdx"}, {"jcamp", "asdf_example.jdx"}, {"mzml", "run.mzML"}, {"mzxml", "run.mzXML"}}) {
    CHECK(test::fuzz_reader<ir::Table>(format, test::fixture("science/chem/" + name), 256) > 0);
  }
}
