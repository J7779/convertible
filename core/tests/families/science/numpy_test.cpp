// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::cell;
using test::science::detected;

namespace {

Json read_npy(const std::string& name, const std::string& format = "npy") {
  const auto data = test::read_as<ir::Data>(format, test::fixture("science/arrays/" + name));
  if (!data) {
    FAIL_CHECK(name, ": ", data.error().message);
    return Json();
  }
  return data->value;
}

const Json& var(const Json& data, std::size_t i = 0) { return data["variables"][i]; }

}  // namespace

TEST_CASE("science/npy: decodes NumPy files of every kind") {
  {
    const Json v = var(read_npy("f8_2x3.npy"));
    CHECK(v["dtype"].as_string() == "float64");
    CHECK(v["shape"] == Json::array({2, 3}));
    CHECK(v["data"] == Json::array({Json::array({0.0, 1.5, 3.0}), Json::array({4.5, 6.0, 7.5})}));
  }
  {
    const Json v = var(read_npy("i2_big_endian.npy"));
    CHECK(v["dtype"].as_string() == "int16");
    CHECK(v["data"] == Json::array({-3, -2, -1, 0, 1, 2, 32767, -32768}));
  }
  {
    const Json v = var(read_npy("u1_fortran.npy"));
    CHECK(v["dtype"].as_string() == "uint8");
    CHECK(v["data"][1] == Json::array({4, 5, 6, 7}));  // C order after transposition
    CHECK(v["data"][2][3].as_int64() == 11);
  }
  {
    const Json v = var(read_npy("c8.npy"));
    CHECK(v["dtype"].as_string() == "complex64");
    CHECK(v["data"][0] == Json::array({1.0, 2.0}));
    CHECK(v["data"][1] == Json::array({-0.5, -0.25}));
  }
  {
    const Json v = var(read_npy("f2.npy"));
    CHECK(v["dtype"].as_string() == "float16");
    CHECK(v["data"][2].as_double() == 65504.0);
    CHECK_APPROX(v["data"][3].as_double(), 6.1e-05, 1e-7);
  }
  CHECK(var(read_npy("bool.npy"))["data"] == Json::array({Json::array({true, false}), Json::array({false, true})}));
  CHECK(var(read_npy("unicode.npy"))["data"] == Json::array({"alpha", "\xCE\xB2" "eta", ""}));
  CHECK(var(read_npy("bytes.npy"))["data"] == Json::array({"ab", "cde"}));
  {
    const Json v = var(read_npy("record_aligned.npy"));
    CHECK(v["dtype"].as_string() == "record");
    CHECK(v["data"][0] == Json::object({{"id", 1}, {"x", 2.5}, {"name", "one"}}));
    CHECK(v["data"][1] == Json::object({{"id", -2}, {"x", -0.125}, {"name", "two"}}));
  }
  CHECK(var(read_npy("i8_3d.npy"))["data"][1][2][3].as_int64() == 11);
  CHECK(var(read_npy("v3_f4.npy"))["data"] == Json::array({1.25, -7.5}));
  CHECK(var(read_npy("v2_u4.npy"))["data"] == Json::array({7, 4294967295ull}));
  CHECK(detected(test::fixture("science/arrays/f8_2x3.npy")) == "npy");
}

TEST_CASE("science/npy: tables, bit-exact round trips and dtype conversion") {
  for (const char* name : {"f8_2x3.npy", "i2_big_endian.npy", "u1_fortran.npy", "c8.npy", "f2.npy", "bool.npy",
                           "unicode.npy", "bytes.npy", "record_aligned.npy", "i8_3d.npy", "v3_f4.npy", "v2_u4.npy"}) {
    INFO(name);
    const auto data = test::read_as<ir::Data>("npy", test::fixture(std::string("science/arrays/") + name));
    REQUIRE_OK(data);
    const auto written = test::write_from("npy", *data);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Data>("npy", *written);
    REQUIRE_OK(back);
    CHECK_MESSAGE(test::data_equal(back->value["variables"][0], data->value["variables"][0]),
                  test::data_diff(back->value["variables"][0], data->value["variables"][0]));
  }
  // Our little-endian C-order file equals NumPy's own for a plain array.
  const auto data = test::read_as<ir::Data>("npy", test::fixture("science/arrays/f8_2x3.npy"));
  REQUIRE_OK(data);
  const auto written = test::write_from("npy", *data);
  REQUIRE_OK(written);
  CHECK(*written == test::fixture("science/arrays/f8_2x3.npy"));
  // Table view.
  const auto table = test::read_as<ir::Table>("npy", test::fixture("science/arrays/f8_2x3.npy"));
  REQUIRE_OK(table);
  CHECK(cell(*table, 1, 2) == "7.5");
  const auto from_table = test::write_from("npy", *table);
  REQUIRE_OK(from_table);
  const auto again = test::read_as<ir::Data>("npy", *from_table);
  REQUIRE_OK(again);
  CHECK(again->value["variables"][0]["data"] == data->value["variables"][0]["data"]);
  // dtype option and Fortran order.
  const auto f32 = test::write_from("npy", *data, R"({"array_dtype":"float32","fortran_order":true})");
  REQUIRE_OK(f32);
  const auto f32_back = test::read_as<ir::Data>("npy", *f32);
  REQUIRE_OK(f32_back);
  CHECK(f32_back->value["variables"][0]["dtype"].as_string() == "float32");
  CHECK(f32_back->value["variables"][0]["data"] == data->value["variables"][0]["data"]);
  CHECK(f32->size() < written->size());
  CHECK_STATUS(test::write_from("npy", *data, R"({"array_dtype":"text"})"), Status::InvalidArgument);
}

TEST_CASE("science/npz: savez and savez_compressed archives") {
  for (const char* name : {"arrays.npz", "arrays_compressed.npz"}) {
    INFO(name);
    const Json data = read_npy(name, "npz");
    REQUIRE(data["variables"].size() == 2);
    CHECK(data["variables"][0]["name"].as_string() == "grid");
    CHECK(data["variables"][0]["data"] == Json::array({Json::array({0.0, 1.0}), Json::array({2.0, 3.0})}));
    CHECK(data["variables"][1]["data"] == Json::array({"x", "y"}));
    CHECK(detected(test::fixture(std::string("science/arrays/") + name)) == "npz");
  }
  const auto data = test::read_as<ir::Data>("npz", test::fixture("science/arrays/arrays.npz"));
  REQUIRE_OK(data);
  const auto stored = test::write_from("npz", *data, R"({"compression_level":0})");
  const auto packed = test::write_from("npz", *data, R"({"compression_level":9})");
  REQUIRE_OK(stored);
  REQUIRE_OK(packed);
  CHECK(packed->size() < stored->size());
  const auto back = test::read_as<ir::Data>("npz", *packed);
  REQUIRE_OK(back);
  CHECK(test::data_equal(back->value, data->value));
  // npz -> npy keeps the first array and warns.
  std::vector<std::string> warnings;
  CHECK_OK(test::write_from("npy", *data, "", &warnings));
  CHECK(warnings.size() == 1);
}

TEST_CASE("science/npy: JSON-shaped data becomes arrays") {
  const auto parsed = util::parse_json(R"({"a":[[1,2],[3,4]],"b":[0.5,1.5],"c":{"dtype":"complex128","data":[[1,2],[3,4]]}})");
  REQUIRE_OK(parsed);
  const auto npz = test::write_from("npz", ir::Data{*parsed});
  REQUIRE_OK(npz);
  const auto back = test::read_as<ir::Data>("npz", *npz);
  REQUIRE_OK(back);
  CHECK(back->value["variables"][0]["dtype"].as_string() == "int64");
  CHECK(back->value["variables"][1]["dtype"].as_string() == "float64");
  CHECK(back->value["variables"][2]["shape"] == Json::array({2}));
  CHECK(back->value["variables"][2]["data"][1] == Json::array({3.0, 4.0}));
}

TEST_CASE("science/npy: hostile input") {
  using namespace std::string_view_literals;
  using namespace std::string_literals;
  CHECK_STATUS(test::read_as<ir::Data>("npy", as_bytes("\x93NUMPY\x01\x00\x30\x00{'descr': '|O', 'fortran_order': False, 'shape': (1,), }   \n"sv)),
               Status::Decode);
  const std::string header = "{'descr': '<f8', 'fortran_order': False, 'shape': (99999999999, 99999999999), }\n";
  std::string bomb = "\x93NUMPY\x01"s + '\0' + static_cast<char>(header.size()) + '\0' + header;
  CHECK_STATUS(test::read_as<ir::Data>("npy", as_bytes(bomb)), Status::Limit);
  for (const char* name : {"f8_2x3.npy", "record_aligned.npy", "u1_fortran.npy", "unicode.npy"}) {
    CHECK(test::fuzz_reader<ir::Data>("npy", test::fixture(std::string("science/arrays/") + name)) > 0);
  }
  CHECK(test::fuzz_reader<ir::Data>("npz", test::fixture("science/arrays/arrays_compressed.npz"), 128) > 0);
}
