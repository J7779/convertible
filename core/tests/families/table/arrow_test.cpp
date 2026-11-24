// Apache Arrow IPC files and streams (Feather 2).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <utility>

#include "engine/detect.hpp"
#include "families/table/table_test_support.hpp"

using namespace cvt;
using namespace cvt::table_test;

TEST_CASE("table/arrow: decodes Feather and IPC streams written by pyarrow") {
  const ir::Table expected = single(make_sheet(
      "", {{S("id"), S("small"), S("u16"), S("ratio"), S("f32"), S("name"), S("flag"), S("day"), S("ts"), S("tod"), S("price"), S("colour"), S("point.x"), S("point.y"), S("tags")},
           {N(1), N(1), N(0), N(0.5), N(1.5), S("alpha"), B(true), D("2024-05-01"), D("2024-05-01T12:30:00"), D("12:30:00"), N(12.34), S("red"), N(1), N(2), S("[\"a\",\"b\"]")},
           {N(2), E(), N(65535), E(), N(-2.25), E(), B(false), E(), D("1999-12-31T23:59:59"), E(), N(-0.01), S("green"), E(), E(), S("[]")},
           {N(3), N(-128), E(), N(-1e-07), E(), S("世界"), E(), D("1960-02-29"), E(), D("00:00:01"), E(), S("red"), N(-1.5), N(0)}}));
  for (const auto& [format, file] : {std::pair<const char*, const char*>{"arrow", "pyarrow_types_lz4.feather"},
                                     {"arrow", "pyarrow_types_plain.feather"},
                                     {"arrows", "pyarrow_types.arrows"}}) {
    INFO(file);
    const auto table = read_fixture(format, file);
    REQUIRE_OK(table);
    CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), expected), test::table_diff(renamed(*table, ""), expected));
    const Bytes bytes = test::fixture(std::string("table/") + file);
    CHECK(detect_format(Registry::instance(), bytes, "")->format->id == format);
    CHECK(test::fuzz_reader<ir::Table>(format, bytes, 300) > 0);
  }
  CHECK(test::convert(test::fixture("table/pyarrow_types_lz4.feather"), "", "xlsx").ok());
  CHECK_STATUS(test::read_as<ir::Table>("arrow", test::bytes("FEA1....")), Status::Decode);
}
