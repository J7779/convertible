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

void check_scipy_variables(const Json& data) {
  const Json* matrix = find_var(data, "matrix");
  REQUIRE(matrix != nullptr);
  CHECK((*matrix)["dtype"].as_string() == "float64");
  CHECK((*matrix)["data"] == Json::array({Json::array({0.0, 0.5, 1.0}), Json::array({1.5, 2.0, 2.5})}));
  const Json* ints = find_var(data, "ints");
  REQUIRE(ints != nullptr);
  CHECK((*ints)["dtype"].as_string() == "int16");
  CHECK((*ints)["data"] == Json::array({Json::array({-1, 2, -3})}));
  const Json* big = find_var(data, "big");
  REQUIRE(big != nullptr);
  CHECK((*big)["data"][0][0].as_uint64() == (1ull << 62) + 1);  // exact, not via double
  const Json* flags = find_var(data, "flags");
  REQUIRE(flags != nullptr);
  CHECK((*flags)["data"] == Json::array({Json::array({true, false, true})}));
  const Json* z = find_var(data, "z");
  REQUIRE(z != nullptr);
  CHECK((*z)["dtype"].as_string() == "complex128");
  CHECK((*z)["data"][0][1] == Json::array({3.0, -4.0}));
  CHECK((*find_var(data, "label"))["data"].as_string() == "synthetic text");
  CHECK((*find_var(data, "lines"))["data"] == Json::array({"ab", "cd"}));
  const Json& cells = *find_var(data, "cells");
  CHECK(cells["class"].as_string() == "cell");
  CHECK(cells["data"][0][0]["data"] == Json::array({Json::array({1.0})}));
  CHECK(cells["data"][0][1]["data"].as_string() == "two");
  CHECK(cells["data"][0][2]["dtype"].as_string() == "uint8");
  const Json& config = *find_var(data, "config");
  CHECK(config["class"].as_string() == "struct");
  CHECK(config["fields"]["gain"]["data"] == Json::array({Json::array({2.5})}));
  CHECK(config["fields"]["name"]["data"].as_string() == "probe");
  CHECK(config["fields"]["inner"]["fields"]["k"]["dtype"].as_string() == "int32");
  const Json& sp = *find_var(data, "sp");
  CHECK(sp["class"].as_string() == "sparse");
  CHECK(sp["row"] == Json::array({1, 0}));
  CHECK(sp["col"] == Json::array({0, 1}));
  CHECK(sp["value"] == Json::array({2.0, 1.5}));
}

}  // namespace

TEST_CASE("science/mat: decodes SciPy level 5 files, compressed and not") {
  for (const char* name : {"v5_uncompressed.mat", "v7_compressed.mat"}) {
    INFO(name);
    const auto data = test::read_as<ir::Data>("mat", test::fixture(std::string("science/arrays/") + name));
    REQUIRE_OK(data);
    check_scipy_variables(data->value);
    CHECK(detected(test::fixture(std::string("science/arrays/") + name)) == "mat");
  }
}

TEST_CASE("science/mat: decodes a level 4 file") {
  const auto data = test::read_as<ir::Data>("mat", test::fixture("science/arrays/v4.mat"));
  REQUIRE_OK(data);
  const Json* m = find_var(data->value, "m");
  REQUIRE(m != nullptr);
  CHECK((*m)["data"] == Json::array({Json::array({1.0, 2.0}), Json::array({3.0, 4.0})}));
  CHECK((*find_var(data->value, "s"))["data"][0][0].as_double() == 5.5);
  CHECK(detected(test::fixture("science/arrays/v4.mat"), "v4.mat") == "mat");
}

TEST_CASE("science/mat: writer round trips every class; compression shrinks") {
  const auto data = test::read_as<ir::Data>("mat", test::fixture("science/arrays/v5_uncompressed.mat"));
  REQUIRE_OK(data);
  const auto packed = test::write_from("mat", *data);
  const auto plain = test::write_from("mat", *data, R"({"compression_level":0})");
  REQUIRE_OK(packed);
  REQUIRE_OK(plain);
  CHECK(packed->size() < plain->size());
  for (const Bytes* bytes : {&*packed, &*plain}) {
    const auto back = test::read_as<ir::Data>("mat", *bytes);
    REQUIRE_OK(back);
    check_scipy_variables(back->value);
  }
  // The header carries no date or platform.
  CHECK(test::str(*packed).substr(0, 19) == "MATLAB 5.0 MAT-file");
  CHECK(test::str(*packed).substr(19, 97) == std::string(97, ' '));
}

TEST_CASE("science/mat: tables and plain JSON") {
  const auto table = test::read_as<ir::Table>("mat", test::fixture("science/arrays/v7_compressed.mat"));
  REQUIRE_OK(table);
  bool found = false;
  for (const auto& sheet : table->sheets) {
    if (sheet.name == "config.inner.k") found = sheet.rows[0][1].number == 2;
  }
  CHECK(found);
  const auto npy = test::read_as<ir::Data>("npy", test::fixture("science/arrays/f8_2x3.npy"));
  REQUIRE_OK(npy);
  const auto mat = test::write_from("mat", *npy);
  REQUIRE_OK(mat);
  const auto back = test::read_as<ir::Data>("mat", *mat);
  REQUIRE_OK(back);
  CHECK(back->value["variables"][0]["data"] == npy->value["variables"][0]["data"]);
  const auto parsed = util::parse_json(R"({"gain":1.5,"label":"x","nested":{"v":[1,2,3]},"mixed":[1,"a"]})");
  REQUIRE_OK(parsed);
  const auto written = test::write_from("mat", ir::Data{*parsed});
  REQUIRE_OK(written);
  const auto read = test::read_as<ir::Data>("mat", *written);
  REQUIRE_OK(read);
  CHECK((*find_var(read->value, "gain"))["data"][0][0].as_double() == 1.5);
  CHECK((*find_var(read->value, "label"))["data"].as_string() == "x");
  CHECK((*find_var(read->value, "nested"))["fields"]["v"]["data"] == Json::array({Json::array({1, 2, 3})}));
  CHECK((*find_var(read->value, "mixed"))["class"].as_string() == "cell");
}

TEST_CASE("science/mat: hostile input") {
  Bytes bomb = test::fixture("science/arrays/v5_uncompressed.mat");
  CHECK(test::fuzz_reader<ir::Data>("mat", bomb, 256) > 0);
  CHECK(test::fuzz_reader<ir::Data>("mat", test::fixture("science/arrays/v7_compressed.mat"), 256) > 0);
  CHECK(test::fuzz_reader<ir::Data>("mat", test::fixture("science/arrays/v4.mat")) > 0);
  CHECK_STATUS(test::read_as<ir::Data>("mat", as_bytes("MATLAB 7.3 MAT-file" + std::string(109, ' '))), Status::Decode);
}
