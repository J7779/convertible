// Stata, SPSS and SAS transport files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>

#include "engine/detect.hpp"
#include "families/table/table_test_support.hpp"

using namespace cvt;
using namespace cvt::table_test;

namespace {

// The data frame of tools/make_data.py.
ir::Table frame(bool with_dates) {
  ir::Sheet sheet = make_sheet("", {{S("id"), S("score"), S("label"), S("when")},
                                    {N(1), N(1.5), S("alpha"), D("2024-05-01T12:30:00")},
                                    {N(2), N(-2.25), S("beta"), D("1999-12-31T00:00:00")},
                                    {N(3), E(), S("gamma"), D("1960-02-29T06:00:00")}});
  if (!with_dates) {
    for (auto& row : sheet.rows) row.resize(3);
  }
  sheet.has_header = true;
  return single(sheet);
}

}  // namespace

TEST_CASE("table/stats: Stata files written by pandas (formats 114 and 118)") {
  for (const char* file : {"pandas_sample_v114.dta", "pandas_sample_v118.dta"}) {
    INFO(file);
    const auto table = read_fixture("dta", file);
    REQUIRE_OK(table);
    CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), frame(true)), test::table_diff(renamed(*table, ""), frame(true)));
    CHECK(test::fuzz_reader<ir::Table>("dta", test::fixture(std::string("table/") + file)) > 0);
  }
  CHECK(detect_format(Registry::instance(), test::fixture("table/pandas_sample_v118.dta"), "")->format->id == "dta");
}

TEST_CASE("table/stats: SPSS file written by pyreadstat") {
  const auto table = read_fixture("sav", "pyreadstat_sample.sav");
  REQUIRE_OK(table);
  CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), frame(true)), test::table_diff(renamed(*table, ""), frame(true)));
  CHECK(test::fuzz_reader<ir::Table>("sav", test::fixture("table/pyreadstat_sample.sav")) > 0);
  CHECK(detect_format(Registry::instance(), test::fixture("table/pyreadstat_sample.sav"), "")->format->id == "sav");
}

TEST_CASE("table/stats: SAS transport file written by pyreadstat") {
  const auto table = read_fixture("xpt", "pyreadstat_sample.xpt");
  REQUIRE_OK(table);
  CHECK(table->sheets[0].name == "SAMPLE");
  CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), frame(false)), test::table_diff(renamed(*table, ""), frame(false)));
  CHECK(test::fuzz_reader<ir::Table>("xpt", test::fixture("table/pyreadstat_sample.xpt")) > 0);
  CHECK(detect_format(Registry::instance(), test::fixture("table/pyreadstat_sample.xpt"), "")->format->id == "xpt");
}
