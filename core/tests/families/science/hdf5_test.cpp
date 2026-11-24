// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::detected;

namespace {

const Json* find_var(const Json& data, std::string_view name) {
  for (const Json& v : data["variables"].elements()) {
    if (v["name"].as_string() == name) return &v;
  }
  return nullptr;
}

void check_common(const Json& data) {
  CHECK(data["attributes"]["title"].as_string() == "synthetic");
  CHECK(data["attributes"]["version"].as_int64() == 2);
  CHECK(data["attributes"]["scales"] == Json::array({0.5, 1.5}));
  CHECK(data["attributes"]["note"].as_string() == "variable text");
  CHECK(data["attributes"]["/grid"]["units"].as_string() == "K");
  const Json* contiguous = find_var(data, "/grid/contiguous");
  REQUIRE(contiguous != nullptr);
  CHECK((*contiguous)["data"] == Json::array({Json::array({0.0, 0.25, 0.5}), Json::array({0.75, 1.0, 1.25})}));
  const Json* chunked = find_var(data, "/grid/chunked");
  REQUIRE(chunked != nullptr);
  CHECK((*chunked)["dtype"].as_string() == "int32");
  for (int r = 0; r < 10; ++r) {
    for (int c = 0; c < 10; ++c) CHECK((*chunked)["data"][r][c].as_int64() == r * 10 + c - 50);
  }
  CHECK((*find_var(data, "/grid/big_endian"))["data"] == Json::array({1, -2, 3}));
  CHECK((*find_var(data, "/grid/sub/scalar"))["data"].as_double() == 2.5);
  CHECK((*find_var(data, "/labels"))["data"] == Json::array({"ab", "cde"}));
  CHECK((*find_var(data, "/names"))["data"] == Json::array({"alpha", "beta"}));
  CHECK((*find_var(data, "/names"))["dtype"].as_string() == "U5");
  const Json& records = *find_var(data, "/records");
  CHECK(records["data"][1] == Json::object({{"id", 2}, {"value", -1.0}, {"tag", "yy"}}));
  CHECK((*find_var(data, "/colours"))["data"] == Json::array({0, 1, 1}));
}

}  // namespace

TEST_CASE("science/hdf5: decodes h5py files with the earliest format (symbol tables, v1 B-tree chunks)") {
  const auto data = test::read_as<ir::Data>("h5", test::fixture("science/arrays/earliest.h5"));
  REQUIRE_OK(data);
  check_common(data->value);
  CHECK((*find_var(data->value, "/compact"))["data"] == Json::array({7, 8, 9}));
  CHECK(detected(test::fixture("science/arrays/earliest.h5")) == "h5");
}

TEST_CASE("science/hdf5: decodes the latest format (dense links and attributes, chunk indexes)") {
  const auto data = test::read_as<ir::Data>("h5", test::fixture("science/arrays/latest.h5"));
  REQUIRE_OK(data);
  check_common(data->value);
  for (int i = 0; i < 20; ++i) {
    const Json* d = find_var(data->value, std::format("/many/d{:02d}", i));
    REQUIRE(d != nullptr);
    CHECK((*d)["data"] == Json::array({i}));
  }
  CHECK(data->value["attributes"]["/many"].size() == 12);
  CHECK(data->value["attributes"]["/many"]["a11"].as_int64() == 11);
  CHECK((*find_var(data->value, "/single_chunk"))["data"][2][3].as_double() == 11.0);
  CHECK((*find_var(data->value, "/fixed_array"))["data"][4][5].as_int64() == 29);
}

TEST_CASE("science/hdf5: MATLAB 7.3 files, cycles and conversions") {
  const auto mat = test::read_as<ir::Data>("mat", test::fixture("science/arrays/mat73.mat"));
  REQUIRE_OK(mat);
  CHECK((*find_var(mat->value, "/m"))["data"] == Json::array({Json::array({1.0, 2.0}), Json::array({3.0, 4.0})}));
  CHECK(detected(test::fixture("science/arrays/mat73.mat"), "m.mat") == "mat");
  std::vector<std::string> warnings;
  const auto cycle = test::read_as<ir::Data>("h5", test::fixture("science/arrays/cycle.h5"), "", &warnings);
  REQUIRE_OK(cycle);
  CHECK(cycle->value["variables"].size() == 1);
  CHECK(warnings.size() == 1);
  // HDF5 -> NumPy archive keeps every dataset.
  const auto converted = test::convert(test::fixture("science/arrays/earliest.h5"), "h5", "npz");
  REQUIRE(converted.ok());
  const auto npz = test::read_as<ir::Data>("npz", converted.data);
  REQUIRE_OK(npz);
  CHECK(npz->value["variables"].size() == 9);
}

TEST_CASE("science/hdf5: hostile input") {
  CHECK(test::fuzz_reader<ir::Data>("h5", test::fixture("science/arrays/earliest.h5"), 96) > 0);
  CHECK(test::fuzz_reader<ir::Data>("h5", test::fixture("science/arrays/latest.h5"), 96) > 0);
  CHECK_STATUS(test::read_as<ir::Data>("h5", as_bytes("\x89HDF\r\n\x1a\n")), Status::Decode);
}
