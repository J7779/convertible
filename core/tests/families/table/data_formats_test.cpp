// SQL scripts, dBase tables and Parquet files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <format>
#include <string>

#include "engine/detect.hpp"
#include "families/table/snappy.hpp"
#include "families/table/table_test_support.hpp"
#include "util/deflate.hpp"

using namespace cvt;
using namespace cvt::table_test;

// --------------------------------------------------------------------------- SQL

TEST_CASE("table/sql: dialects quote, type and escape correctly") {
  ir::Sheet sheet = make_sheet("My \"Table\"", {{S("id"), S("name"), S("ok"), S("day"), S("when"), S("ratio")},
                                                {N(1), S("O'Brien \\ back"), B(true), D("2024-05-01"), D("2024-05-01T12:30:00"), N(0.5)},
                                                {N(2), E(), B(false), E(), E(), E()}});
  sheet.has_header = true;
  const ir::Table table = single(sheet);
  const auto text = [&](std::string_view options) { return test::str(test::write_from("sql", table, options).value()); };
  const std::string sqlite = text({});
  CHECK(sqlite.find("CREATE TABLE \"My \"\"Table\"\"\" (\n  \"id\" INTEGER,\n  \"name\" TEXT,\n  \"ok\" BOOLEAN,\n  \"day\" DATE,\n  \"when\" DATETIME,\n  \"ratio\" REAL\n);") != std::string::npos);
  CHECK(sqlite.find("(1, 'O''Brien \\ back', 1, '2024-05-01', '2024-05-01 12:30:00', 0.5),") != std::string::npos);
  CHECK(sqlite.find("(2, NULL, 0, NULL, NULL, NULL);") != std::string::npos);
  CHECK(sqlite.starts_with("BEGIN;"));
  const std::string mysql = text(R"({"sql_dialect":"mysql"})");
  CHECK(mysql.find("CREATE TABLE `My \"Table\"`") != std::string::npos);
  CHECK(mysql.find("'O''Brien \\\\ back', TRUE") != std::string::npos);
  CHECK(mysql.find("`id` BIGINT") != std::string::npos);
  CHECK(mysql.find("`ratio` DOUBLE") != std::string::npos);
  const std::string postgres = text(R"({"sql_dialect":"postgres","drop_table":true})");
  CHECK(postgres.find("DROP TABLE IF EXISTS \"My \"\"Table\"\"\";") != std::string::npos);
  CHECK(postgres.find("\"when\" TIMESTAMP") != std::string::npos);
  CHECK(postgres.find("\"ratio\" DOUBLE PRECISION") != std::string::npos);
  CHECK(postgres.find("'O''Brien \\ back', TRUE") != std::string::npos);
  CHECK(text(R"({"create_table":false})").find("CREATE") == std::string::npos);
  const std::string batched = text(R"({"batch_size":1})");
  std::size_t inserts = 0;
  for (std::size_t at = batched.find("INSERT"); at != std::string::npos; at = batched.find("INSERT", at + 1)) ++inserts;
  CHECK(inserts == 2);
}

TEST_CASE("table/sql: scripts read back in every dialect") {
  ir::Table table = sample_workbook(true);
  table.sheets.resize(1);
  table.sheets[0].has_header = true;
  for (const char* dialect : {"sqlite", "mysql", "postgres"}) {
    INFO(dialect);
    const auto back = round_trip("sql", table, std::format(R"({{"sql_dialect":"{}","batch_size":2}})", dialect));
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::tables_equal(*back, table), test::table_diff(*back, table));
  }
}

TEST_CASE("table/sql: reads INSERT dumps and PostgreSQL COPY blocks") {
  const std::string mysql = R"(-- MySQL dump
/*!40101 SET NAMES utf8mb4 */;
DROP TABLE IF EXISTS `t`;
CREATE TABLE `t` (
  `id` int NOT NULL AUTO_INCREMENT,
  `name` varchar(20) DEFAULT NULL,
  `made` date,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB;
INSERT INTO `t` VALUES (1,'it\'s','2024-05-01'),(2,'back\\slash\nline',NULL),(3,'fn',NOW());
INSERT INTO `t` (`name`,`id`) VALUES ('swapped',-4);
)";
  const auto table = test::read_as<ir::Table>("sql", as_bytes(mysql));
  REQUIRE_OK(table);
  const ir::Table expected = single(make_sheet("t", {{S("id"), S("name"), S("made")},
                                                     {N(1), S("it's"), D("2024-05-01")},
                                                     {N(2), S("back\\slash\nline")},
                                                     {N(3), S("fn"), S("NOW")},
                                                     {N(-4), S("swapped")}}));
  CHECK_MESSAGE(test::tables_equal(*table, expected), test::table_diff(*table, expected));
  const std::string pg = "CREATE TABLE public.p (n integer, label text, flag boolean);\n"
                         "COPY public.p (n, label, flag) FROM stdin;\n1\tone\tt\n2\t\\N\tf\n3\ttab\\there\tt\n\\.\n";
  const auto copied = test::read_as<ir::Table>("sql", as_bytes(pg));
  REQUIRE_OK(copied);
  const ir::Table expected_pg = single(make_sheet("p", {{S("n"), S("label"), S("flag")},
                                                        {N(1), S("one"), B(true)},
                                                        {N(2), E(), B(false)},
                                                        {N(3), S("tab\there"), B(true)}}));
  CHECK_MESSAGE(test::tables_equal(*copied, expected_pg), test::table_diff(*copied, expected_pg));
  CHECK_STATUS(test::read_as<ir::Table>("sql", test::bytes("SELECT 1;")), Status::Decode);
  CHECK(test::fuzz_reader<ir::Table>("sql", as_bytes(mysql)) > 0);
}

// --------------------------------------------------------------------------- dBase

TEST_CASE("table/dbf: decodes dBase III and Visual FoxPro tables written by the dbf package") {
  const auto dbase = read_fixture("dbf", "dbfpy_dbase3.dbf");
  REQUIRE_OK(dbase);
  const ir::Table expected = single(make_sheet("", {{S("NAME"), S("QTY"), S("PRICE"), S("BORN"), S("OK")},
                                                    {S("Tea"), N(3), N(1.5), D("2024-05-01"), B(true)},
                                                    {S("Café"), N(-12), N(0.99), D("1999-12-31"), B(false)},
                                                    {E(), N(0), N(0)}}));  // the deleted record is skipped
  CHECK_MESSAGE(test::tables_equal(renamed(*dbase, ""), expected), test::table_diff(renamed(*dbase, ""), expected));
  const auto fox = read_fixture("dbf", "dbfpy_foxpro.dbf");
  REQUIRE_OK(fox);
  const ir::Table expected_fox = single(make_sheet("", {{S("NAME"), S("COUNT"), S("AMOUNT"), S("STAMP"), S("RATIO"), S("D")},
                                                        {S("alpha"), N(7), N(12.3456), D("2024-05-01T12:30:15"), N(2.5), D("2024-05-01")},
                                                        {S("beta"), N(-1), N(-0.5), D("2000-01-01T00:00:00"), N(-0.001), D("1970-01-01")}}));
  CHECK_MESSAGE(test::tables_equal(renamed(*fox, ""), expected_fox), test::table_diff(renamed(*fox, ""), expected_fox));
  const auto named = test::read_as<ir::Table>("dbf", test::fixture("table/dbfpy_dbase3.dbf"));
  REQUIRE_OK(named);
  CHECK(named->sheets[0].has_header);
}

TEST_CASE("table/dbf: write and read back with field types and code pages") {
  ir::Sheet sheet = make_sheet("t", {{S("name"), S("qty"), S("price"), S("born"), S("ok"), S("when"), S("big")},
                                     {S("Tea"), N(3), N(1.5), D("2024-05-01"), B(true), D("2024-05-01T12:30:00"), N(1e20)},
                                     {S("Café"), N(-12), N(0.125), D("1999-12-31"), B(false), E(), N(2)},
                                     {E(), E(), E(), E(), E(), E(), E()}});
  sheet.has_header = true;
  const auto bytes = test::write_from("dbf", single(sheet));
  REQUIRE_OK(bytes);
  CHECK(std::to_integer<int>((*bytes)[0]) == 0x03);
  CHECK(std::to_integer<int>((*bytes)[29]) == 0x03);  // Windows ANSI language driver
  const auto back = test::read_as<ir::Table>("dbf", *bytes);
  REQUIRE_OK(back);
  ir::Table expected = single(sheet);
  expected.sheets[0].rows[0] = {S("NAME"), S("QTY"), S("PRICE"), S("BORN"), S("OK"), S("WHEN"), S("BIG")};
  expected.sheets[0].rows[1][5] = S("2024-05-01T12:30:00");  // dBase III has no date-time type
  expected.sheets[0].rows[1][6] = S("1e+20");  // too wide for N: kept as text
  expected.sheets[0].rows[2][6] = S("2");
  CHECK_MESSAGE(test::tables_equal(renamed(*back, "t"), expected), test::table_diff(renamed(*back, "t"), expected));
  const auto dos = test::write_from("dbf", single(sheet), R"({"dbf_encoding":"cp437"})");
  REQUIRE_OK(dos);
  CHECK(std::to_integer<int>((*dos)[29]) == 0x01);
  const auto dos_back = test::read_as<ir::Table>("dbf", *dos);
  REQUIRE_OK(dos_back);
  CHECK(dos_back->sheets[0].rows[2][0].text == "Café");
  CHECK_STATUS(test::write_from("dbf", single(sheet), R"({"dbf_encoding":"klingon"})"), Status::InvalidArgument);
}

TEST_CASE("table/dbf: damaged files fail cleanly") {
  const Bytes valid = test::fixture("table/dbfpy_foxpro.dbf");
  CHECK_STATUS(test::read_as<ir::Table>("dbf", ByteView(valid).first(10)), Status::Decode);
  CHECK(test::fuzz_reader<ir::Table>("dbf", valid) > 0);
  CHECK(test::fuzz_reader<ir::Table>("dbf", test::fixture("table/dbfpy_dbase3.dbf")) > 0);
  CHECK(detect_format(Registry::instance(), valid, "")->format->id == "dbf");
}

// --------------------------------------------------------------------------- Parquet

namespace {

ir::Table pyarrow_types(std::string name) {
  return single(make_sheet(std::move(name),
                           {
                               {S("id"), S("small"), S("u32"), S("ratio"), S("f32"), S("name"), S("flag"), S("day"), S("ts_ms"), S("ts_us"), S("tod"), S("price"), S("big_dec")},
                               {N(1), N(1), N(0), N(0.5), N(1.5), S("alpha"), B(true), D("2024-05-01"), D("2024-05-01T12:30:00"), D("2024-05-01T12:30:00.250"), D("12:30:00"), N(12.34), N(123456789012345678.901)},
                               {N(2), N(-2), N(1), N(-1.25), E(), S("Grüße"), B(false), D("1970-01-01"), E(), E(), D("00:00:01"), N(-0.01), E()},
                               {N(3), E(), N(4000000000.0), E(), N(-2.25), E(), E(), E(), D("2000-01-01T00:00:01"), E(), E(), E(), N(-1)},
                               {N(4), N(4), E(), N(3.0e10), N(0), S("世界"), B(true), D("1999-12-31"), D("1960-02-29T06:00:00"), E(), D("23:59:59"), N(99999.99), N(0.001)},
                               {N(5), N(127), N(7), N(1e-07), N(100), S(""), B(false), D("2100-02-28"), D("2024-02-29T23:59:59"), D("1970-01-01T00:00:00"), D("06:00:00"), N(0), N(5)},
                           }));
}

}  // namespace

TEST_CASE("table/parquet: decodes files written by pyarrow with every encoding and compression") {
  for (const char* file : {"pyarrow_types_snappy.parquet", "pyarrow_types_gzip.parquet", "pyarrow_types_none_v2.parquet",
                           "pyarrow_types_plain_nodict.parquet", "pyarrow_types_delta.parquet"}) {
    INFO(file);
    const auto table = read_fixture("parquet", file);
    REQUIRE_OK(table);
    const ir::Table expected = pyarrow_types("");
    CHECK_MESSAGE(test::tables_equal(renamed(*table, ""), expected), test::table_diff(renamed(*table, ""), expected));
    CHECK(table->sheets[0].rows[1][12].text == "123456789012345678.901");  // decimals keep every digit
  }
  const auto delta = read_fixture("parquet", "pyarrow_delta_length.parquet");
  REQUIRE_OK(delta);
  CHECK(test::tables_equal(renamed(*delta, ""), single(make_sheet("", {{S("id"), S("name")}, {N(1), S("alpha")}, {N(2), S("Grüße")}, {N(3)}, {N(4), S("世界")}, {N(5), S("")}}))));
  const auto int96 = read_fixture("parquet", "pyarrow_int96.parquet");
  REQUIRE_OK(int96);
  CHECK(test::tables_equal(renamed(*int96, ""), single(make_sheet("", {{S("id"), S("ts_ms")}, {N(1), D("2024-05-01T12:30:00")}, {N(2)}, {N(3), D("2000-01-01T00:00:01")}, {N(4), D("1960-02-29T06:00:00")}, {N(5), D("2024-02-29T23:59:59")}}))));
}

TEST_CASE("table/parquet: row groups and nested columns") {
  const auto groups = read_fixture("parquet", "pyarrow_rowgroups.parquet");
  REQUIRE_OK(groups);
  REQUIRE(groups->sheets[0].rows.size() == 3001);
  const char* colours[] = {"red", "green", "blue"};
  bool all = true;
  for (int i = 0; i < 3000; ++i) {
    all = all && groups->sheets[0].rows[static_cast<std::size_t>(i) + 1][0].number == i &&
          groups->sheets[0].rows[static_cast<std::size_t>(i) + 1][1].text == colours[i % 3];
  }
  CHECK(all);
  const auto nested = read_fixture("parquet", "pyarrow_nested.parquet");
  REQUIRE_OK(nested);
  const ir::Table expected = single(make_sheet("", {{S("id"), S("point.x"), S("point.y"), S("tags")},
                                                    {N(1), N(1), N(2), S("[\"a\",\"b\"]")},
                                                    {N(2), E(), E(), S("[]")},
                                                    {N(3), N(-1.5), N(0)}}));
  CHECK_MESSAGE(test::tables_equal(renamed(*nested, ""), expected), test::table_diff(renamed(*nested, ""), expected));
}

TEST_CASE("table/parquet: write and read back with each compression") {
  ir::Sheet sheet = types_sheet();
  sheet.has_header = true;
  sheet.rows[3][8] = S(std::string(5000, 'r'));  // compressible
  const ir::Table table = single(sheet);
  std::size_t sizes[3] = {};
  int k = 0;
  for (const char* compression : {"none", "snappy", "gzip"}) {
    INFO(compression);
    const auto bytes = test::write_from("parquet", table, std::format(R"({{"parquet_compression":"{}"}})", compression));
    REQUIRE_OK(bytes);
    sizes[k++] = bytes->size();
    const auto back = test::read_as<ir::Table>("parquet", *bytes);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::tables_equal(renamed(*back, "Types"), table), test::table_diff(renamed(*back, "Types"), table));
  }
  CHECK(sizes[1] < sizes[0]);
  CHECK(sizes[2] < sizes[1]);
}

TEST_CASE("table/parquet: damaged files fail cleanly") {
  const Bytes valid = test::fixture("table/pyarrow_types_snappy.parquet");
  CHECK_STATUS(test::read_as<ir::Table>("parquet", ByteView(valid).first(100)), Status::Decode);
  for (const char* file : {"pyarrow_types_snappy.parquet", "pyarrow_types_delta.parquet", "pyarrow_nested.parquet", "pyarrow_types_none_v2.parquet"}) {
    INFO(file);
    CHECK(test::fuzz_reader<ir::Table>("parquet", test::fixture(std::string("table/") + file), 200) > 0);
  }
  CHECK(detect_format(Registry::instance(), valid, "")->format->id == "parquet");
}

TEST_CASE("table/snappy: blocks round-trip and damaged blocks are rejected") {
  for (const Bytes& data : {Bytes{}, test::bytes("a"), test::random_bytes(10000, 3), test::bytes(std::string(100000, 'x')),
                            test::bytes("abcabcabcabcabcabcabcabcabcabcabcabcabc0123456789abcabcabc")}) {
    const Bytes packed = table::snappy_compress(data);
    const auto unpacked = table::snappy_decompress(packed, 1 << 20);
    REQUIRE_OK(unpacked);
    CHECK(*unpacked == data);
  }
  const Bytes repetitive = test::bytes(std::string(100000, 'x'));
  CHECK(table::snappy_compress(repetitive).size() < 6000);
  // Known stream: length 6, literal "ab", copy offset 2 length 4 -> "ababab".
  const auto known = table::snappy_decompress(test::bytes("\x06\x04" "ab\x01\x02"), 100);
  REQUIRE_OK(known);
  CHECK(test::str(*known) == "ababab");
  CHECK_STATUS(table::snappy_decompress(test::bytes("\x06\x04" "ab\x01\x09"), 100), Status::Decode);   // offset too far back
  CHECK_STATUS(table::snappy_decompress(test::bytes("\x05\x04" "ab\x01\x02"), 100), Status::Decode);   // longer than declared
  CHECK_STATUS(table::snappy_decompress(test::bytes("\xFF\xFF\xFF\x0F"), 100), Status::Limit);
}
