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

void check_grid(const Json& data, bool cdf5) {
  CHECK(data["attributes"]["title"].as_string() == "synthetic grid");
  CHECK(data["attributes"]["version"].as_int64() == 3);
  CHECK(data["attributes"]["_dimensions"]["time"].as_int64() == 2);
  const Json& time = *find_var(data, "time");
  CHECK(time["data"] == Json::array({0.0, 60.0}));
  CHECK(time["attributes"]["units"].as_string() == "s");
  const Json& temp = *find_var(data, "temp");
  CHECK(temp["dtype"].as_string() == "float32");
  CHECK(temp["shape"] == Json::array({2, 2, 3}));
  CHECK(temp["data"][1][1][2].as_double() == 11.5);
  CHECK(temp["attributes"]["factors"] == Json::array({1.0, 2.0}));
  CHECK(temp["attributes"]["_dimensions"] == Json::array({"time", "y", "x"}));
  CHECK((*find_var(data, "grid"))["data"] == Json::array({Json::array({1, -2, 3}), Json::array({-4, 5, -6})}));
  CHECK((*find_var(data, "names"))["data"] == Json::array({"ab", "wxyz"}));
  if (cdf5) {
    const Json& counts = *find_var(data, "counts");
    CHECK(counts["dtype"].as_string() == "uint64");
    CHECK(counts["data"][2].as_uint64() == (1ull << 63));
  }
}

}  // namespace

TEST_CASE("science/netcdf: decodes netCDF4-python classic, 64-bit offset and CDF-5 files") {
  for (const char* name : {"classic.nc", "offset64.nc", "cdf5.nc"}) {
    INFO(name);
    const auto data = test::read_as<ir::Data>("netcdf", test::fixture(std::string("science/arrays/") + name));
    REQUIRE_OK(data);
    check_grid(data->value, std::string_view(name) == "cdf5.nc");
    CHECK(detected(test::fixture(std::string("science/arrays/") + name)) == "netcdf");
  }
}

TEST_CASE("science/netcdf: writer round trips and picks the format") {
  for (const char* name : {"classic.nc", "cdf5.nc"}) {
    INFO(name);
    const auto data = test::read_as<ir::Data>("netcdf", test::fixture(std::string("science/arrays/") + name));
    REQUIRE_OK(data);
    const auto written = test::write_from("netcdf", *data);
    REQUIRE_OK(written);
    CHECK(static_cast<int>((*written)[3]) == (std::string_view(name) == "cdf5.nc" ? 5 : 1));
    const auto back = test::read_as<ir::Data>("netcdf", *written);
    REQUIRE_OK(back);
    check_grid(back->value, std::string_view(name) == "cdf5.nc");
  }
  const auto data = test::read_as<ir::Data>("netcdf", test::fixture("science/arrays/classic.nc"));
  REQUIRE_OK(data);
  for (const auto& [option, version] : std::vector<std::pair<std::string, int>>{{"classic", 1}, {"64bit-offset", 2}, {"cdf5", 5}}) {
    const auto written = test::write_from("netcdf", *data, R"({"netcdf_format":")" + option + "\"}");
    REQUIRE_OK(written);
    CHECK(static_cast<int>((*written)[3]) == version);
  }
  // NumPy -> NetCDF and back.
  const auto npz = test::read_as<ir::Data>("npz", test::fixture("science/arrays/arrays.npz"));
  REQUIRE_OK(npz);
  const auto nc = test::write_from("netcdf", *npz);
  REQUIRE_OK(nc);
  const auto back = test::read_as<ir::Data>("netcdf", *nc);
  REQUIRE_OK(back);
  CHECK((*find_var(back->value, "grid"))["data"] == npz->value["variables"][0]["data"]);
  CHECK((*find_var(back->value, "labels"))["data"] == Json::array({"x", "y"}));
  // Table view.
  const auto table = test::read_as<ir::Table>("netcdf", test::fixture("science/arrays/classic.nc"));
  REQUIRE_OK(table);
  CHECK(table->sheets[2].name == "grid");
}

TEST_CASE("science/netcdf: hostile input") {
  CHECK(test::fuzz_reader<ir::Data>("netcdf", test::fixture("science/arrays/classic.nc"), 512) > 0);
  CHECK(test::fuzz_reader<ir::Data>("netcdf", test::fixture("science/arrays/cdf5.nc"), 256) > 0);
  Bytes bad = test::fixture("science/arrays/classic.nc");
  bad[3] = std::byte{9};
  CHECK_STATUS(test::read_as<ir::Data>("netcdf", bad), Status::Decode);
}
