// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::detected;

namespace {

void check_volume(const Json& v, double scale = 1.0, double offset = 0.0) {
  REQUIRE(v["shape"] == Json::array({2, 3, 4}));
  for (int z = 0; z < 2; ++z) {
    for (int y = 0; y < 3; ++y) {
      for (int x = 0; x < 4; ++x) {
        INFO(x, ",", y, ",", z);
        CHECK(v["data"][z][y][x].as_double() == scale * (x + 10 * y + 100 * z) + offset);
      }
    }
  }
}

Json volume(const std::string& format, const std::string& file) {
  const auto data = test::read_as<ir::Data>(format, test::fixture("science/arrays/" + file));
  if (!data) {
    FAIL_CHECK(file, ": ", data.error().message);
    return Json();
  }
  return data->value["variables"][0];
}

}  // namespace

TEST_CASE("science/volumes: decodes nibabel, pynrrd and SimpleITK volumes") {
  check_volume(volume("nifti", "vol.nii"));
  check_volume(volume("nii.gz", "vol.nii.gz"));
  check_volume(volume("nifti", "scaled.nii"), 0.5, -1.0);
  check_volume(volume("nrrd", "vol_gzip.nrrd"));
  check_volume(volume("nrrd", "vol_raw.nrrd"));
  check_volume(volume("nrrd", "vol_ascii.nrrd"));
  check_volume(volume("mha", "vol.mha"));
  check_volume(volume("mha", "vol_zlib.mha"));
  const Json nii = volume("nifti", "vol.nii");
  CHECK(nii["dtype"].as_string() == "int16");
  CHECK(nii["attributes"]["pixdim"] == Json::array({0.5, 0.5, 2.0}));
  CHECK(nii["attributes"]["descrip"].as_string() == "synthetic");
  CHECK(volume("mha", "vol.mha")["attributes"]["ElementSpacing"].as_string() == "0.5 0.5 2");
  // An Analyze header alone gives its fields and a clear warning.
  std::vector<std::string> warnings;
  const auto analyze = test::read_as<ir::Data>("nifti", test::fixture("science/arrays/analyze.hdr"), "", &warnings);
  REQUIRE_OK(analyze);
  CHECK(analyze->value["variables"][0]["attributes"]["format"].as_string() == "Analyze 7.5");
  CHECK(analyze->value["variables"][0]["attributes"]["dim"] == Json::array({4, 3, 2}));
  CHECK(warnings.size() == 1);
  CHECK(detected(test::fixture("science/arrays/vol.nii"), "vol.nii") == "nifti");
  CHECK(detected(test::fixture("science/arrays/vol.nii.gz"), "vol.nii.gz") == "nii.gz");
  CHECK(detected(test::fixture("science/arrays/vol_gzip.nrrd")) == "nrrd");
  CHECK(detected(test::fixture("science/arrays/vol.mha")) == "mha");
}

TEST_CASE("science/volumes: slices render as frames") {
  const auto image = test::read_as<ir::Image>("nrrd", test::fixture("science/arrays/vol_gzip.nrrd"));
  REQUIRE_OK(image);
  REQUIRE(image->frames.size() == 2);
  CHECK(image->width() == 4);
  CHECK(image->height() == 3);
  // Range 0..123: voxel (3,2,1) = 123 is white, (0,0,0) black; no flip for NRRD.
  CHECK(image->frames[1].rgba[image->frames[1].offset(3, 2)] == 255);
  CHECK(image->frames[0].rgba[0] == 0);
  CHECK(image->frames[0].rgba[image->frames[0].offset(1, 0)] == 2);  // 1/123*255 = 2.07
  // NIfTI is shown with the first row at the bottom.
  const auto nifti = test::read_as<ir::Image>("nifti", test::fixture("science/arrays/vol.nii"));
  REQUIRE_OK(nifti);
  CHECK(nifti->frames[0].rgba[nifti->frames[0].offset(0, 2)] == 0);
}

TEST_CASE("science/volumes: writers round trip arrays and images") {
  const Json source = volume("nifti", "vol.nii");
  ir::Data data{Json::object({{"variables", Json::array({source})}})};
  for (const char* format : {"nifti", "nii.gz", "nrrd", "mha"}) {
    INFO(format);
    const auto written = test::write_from(format, data);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Data>(format, *written);
    REQUIRE_OK(back);
    check_volume(back->value["variables"][0]);
    CHECK(back->value["variables"][0]["dtype"].as_string() == "int16");
  }
  const auto nrrd_text = test::write_from("nrrd", data, R"({"nrrd_encoding":"ascii"})");
  const auto nrrd_raw = test::write_from("nrrd", data, R"({"nrrd_encoding":"raw"})");
  REQUIRE_OK(nrrd_text);
  REQUIRE_OK(nrrd_raw);
  CHECK(test::str(*nrrd_text).find("encoding: ascii") != std::string::npos);
  CHECK_OK(test::read_as<ir::Data>("nrrd", *nrrd_raw));
  // Picture stack -> volume -> picture stack is exact for grey slices.
  ir::Image stack;
  for (int k = 0; k < 3; ++k) {
    ir::Frame f = test::gradient_frame(10, 6);
    for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
      f.rgba[i] = static_cast<std::uint8_t>((f.rgba[i] + 40 * k) % 256);
      f.rgba[i + 1] = f.rgba[i + 2] = f.rgba[i];
      f.rgba[i + 3] = 255;
    }
    f.rgba[0] = f.rgba[1] = f.rgba[2] = 0;
    f.rgba[4] = f.rgba[5] = f.rgba[6] = 255;
    stack.frames.push_back(f);
  }
  for (const char* format : {"nifti", "nrrd", "mha"}) {
    INFO(format);
    const auto written = test::write_from(format, stack);
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Image>(format, *written);
    REQUIRE_OK(back);
    REQUIRE(back->frames.size() == 3);
    for (std::size_t k = 0; k < 3; ++k) CHECK(test::pixels_equal(back->frames[k], stack.frames[k]));
  }
  // The NIfTI description is dropped unless metadata is kept.
  const auto stripped = test::write_from("nifti", data);
  const auto kept = test::write_from("nifti", data, R"({"strip_metadata":false})");
  REQUIRE_OK(stripped);
  REQUIRE_OK(kept);
  CHECK(test::str(*stripped).find("synthetic") == std::string::npos);
  CHECK(test::str(*kept).find("synthetic") != std::string::npos);
}

TEST_CASE("science/volumes: hostile input") {
  for (const auto& [format, name] : std::vector<std::pair<std::string, std::string>>{
           {"nifti", "vol.nii"}, {"nii.gz", "vol.nii.gz"}, {"nrrd", "vol_gzip.nrrd"}, {"nrrd", "vol_ascii.nrrd"},
           {"mha", "vol_zlib.mha"}}) {
    CHECK(test::fuzz_reader<ir::Data>(format, test::fixture("science/arrays/" + name)) > 0);
    CHECK(test::fuzz_reader<ir::Image>(format, test::fixture("science/arrays/" + name), 64) > 0);
  }
  CHECK_STATUS(test::read_as<ir::Data>("nrrd", as_bytes("NRRD0004\ntype: double\nsizes: 100000 100000 100000\nencoding: raw\n\n")),
               Status::Limit);
  CHECK_STATUS(test::read_as<ir::Data>("mha", as_bytes("NDims = 2\nDimSize = 2 2\nElementType = MET_UCHAR\nElementDataFile = other.raw\n")),
               Status::Decode);
}
