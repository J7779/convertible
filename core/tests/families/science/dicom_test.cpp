// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>

#include "families/science/science_test.hpp"

using namespace cvt;
using test::science::detected;

namespace {

Bytes dcm(const std::string& name) { return test::fixture("science/dicom/" + name); }

std::uint8_t ct_expected(int k) {
  // Modality value 20k - 1124; window centre -674, width 400 (PS3.3 linear).
  const double low = -674 - 0.5 - 199.5, high = -674 - 0.5 + 199.5;
  const double t = std::clamp((20.0 * k - 1124 - low) / (high - low), 0.0, 1.0);
  return static_cast<std::uint8_t>(std::lround(t * 255));
}

}  // namespace

TEST_CASE("science/dicom: explicit, implicit, big-endian and deflated CT give the same picture") {
  for (const char* name : {"ct_explicit_le.dcm", "ct_implicit_le.dcm", "ct_explicit_be.dcm", "ct_deflated_undefined_seq.dcm"}) {
    INFO(name);
    const auto image = test::read_as<ir::Image>("dicom", dcm(name));
    REQUIRE_OK(image);
    REQUIRE(image->width() == 8);
    REQUIRE(image->height() == 6);
    for (int k = 0; k < 48; ++k) CHECK(image->frames[0].rgba[static_cast<std::size_t>(k) * 4] == ct_expected(k));
    CHECK(detected(dcm(name)) == "dicom");
  }
  // The data range instead of the stored window.
  const auto full = test::read_as<ir::Image>("dicom", dcm("ct_explicit_le.dcm"), R"({"dicom_window":"auto"})");
  REQUIRE_OK(full);
  CHECK(full->frames[0].rgba[0] == 0);
  CHECK(full->frames[0].rgba[47 * 4] == 255);
}

TEST_CASE("science/dicom: tags as data, identifying elements removed by default") {
  std::vector<std::string> warnings;
  const auto data = test::read_as<ir::Data>("dicom", dcm("ct_deflated_undefined_seq.dcm"), "", &warnings);
  REQUIRE_OK(data);
  const Json& ds = data->value["dataset"];
  CHECK(ds["Rows"].as_int64() == 6);
  CHECK(ds["WindowCenter"].as_double() == -674);
  CHECK(ds["Manufacturer"].as_string() == "Synthetic");
  CHECK(ds["ReferencedImageSequence"][0]["ReferencedSOPClassUID"].as_string() == "1.2.840.10008.5.1.4.1.1.2");
  CHECK_FALSE(ds.contains("PatientName"));
  CHECK_FALSE(ds.contains("PatientID"));
  CHECK_FALSE(ds.contains("InstitutionName"));
  CHECK_FALSE(ds.contains("SOPInstanceUID"));
  CHECK_FALSE(ds.contains("(0009,1010)"));
  CHECK_FALSE(ds["ReferencedImageSequence"][0].contains("ReferencedSOPInstanceUID"));
  CHECK(util::dump_json(data->value).find("Test^Patient") == std::string::npos);
  CHECK(warnings.size() == 1);
  const auto kept = test::read_as<ir::Data>("dicom", dcm("ct_implicit_le.dcm"), R"({"strip_metadata":false})");
  REQUIRE_OK(kept);
  CHECK(kept->value["dataset"]["PatientName"].as_string() == "Test^Patient");
  CHECK(kept->value["dataset"]["(0009,1010)"].as_string() == "private value");
  CHECK(kept->value["file_meta"]["TransferSyntaxUID"].as_string() == "1.2.840.10008.1.2");
}

TEST_CASE("science/dicom: RLE multi-frame, RGB planar, MONOCHROME1 and palette colour") {
  const auto plain = test::read_as<ir::Image>("dicom", dcm("mf_uncompressed.dcm"), R"({"window_low":0,"window_high":255})");
  const auto rle = test::read_as<ir::Image>("dicom", dcm("mf_rle.dcm"), R"({"window_low":0,"window_high":255})");
  REQUIRE_OK(plain);
  REQUIRE_OK(rle);
  REQUIRE(rle->frames.size() == 3);
  CHECK(test::pixels_equal(*plain, *rle));
  CHECK(rle->frames[2].rgba[19 * 4] == 57);  // 19 * 3
  const auto rgb = test::read_as<ir::Image>("dicom", dcm("rgb_planar.dcm"));
  REQUIRE_OK(rgb);
  const std::vector<std::uint8_t> expected = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
                                              10, 40, 70, 255, 20, 50, 80, 255, 30, 60, 90, 255};
  CHECK(rgb->frames[0].rgba == expected);
  const auto mono1 = test::read_as<ir::Image>("dicom", dcm("mono1.dcm"));
  REQUIRE_OK(mono1);
  CHECK(mono1->frames[0].rgba[0] == 255);
  CHECK(mono1->frames[0].rgba[8] == 0);
  const auto palette = test::read_as<ir::Image>("dicom", dcm("palette.dcm"));
  REQUIRE_OK(palette);
  const std::vector<std::uint8_t> colours = {0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255};
  CHECK(palette->frames[0].rgba == colours);
}

TEST_CASE("science/dicom: JPEG frames go through the registry's JPEG codec when one exists") {
  const auto image = test::read_as<ir::Image>("dicom", dcm("jpeg_baseline.dcm"));
  if (!Registry::instance().has_reader("jpg")) {
    CHECK_STATUS(image, Status::Decode);
    SKIP("no JPEG decoder registered in this build");
  }
  REQUIRE_OK(image);
  ir::Frame expected = ir::Frame::filled(8, 8, {0, 0, 0, 255});
  for (std::uint32_t i = 0; i < 64; ++i) expected.rgba[i * 4] = expected.rgba[i * 4 + 1] = expected.rgba[i * 4 + 2] = static_cast<std::uint8_t>(i * 4);
  CHECK(test::psnr(image->frames[0], expected) > 35);
}

TEST_CASE("science/dicom: secondary capture writer") {
  ir::Image grey;
  grey.frames.push_back(ir::Frame::filled(5, 3, {0, 0, 0, 255}));
  for (std::size_t i = 0; i < 15; ++i) grey.frames[0].rgba[i * 4] = grey.frames[0].rgba[i * 4 + 1] = grey.frames[0].rgba[i * 4 + 2] = static_cast<std::uint8_t>(i * 17);
  const auto written = test::write_from("dicom", grey);
  REQUIRE_OK(written);
  CHECK(test::str(*written).substr(128, 4) == "DICM");
  const auto back = test::read_as<ir::Image>("dicom", *written, R"({"window_low":0,"window_high":255})");
  REQUIRE_OK(back);
  CHECK(test::pixels_equal(*back, grey));
  const auto tags = test::read_as<ir::Data>("dicom", *written, R"({"strip_metadata":false})");
  REQUIRE_OK(tags);
  CHECK(tags->value["dataset"]["SOPClassUID"].as_string() == "1.2.840.10008.5.1.4.1.1.7");
  CHECK(tags->value["dataset"]["PatientName"].as_string().empty());
  CHECK(tags->value["dataset"]["SOPInstanceUID"].as_string().starts_with("2.25."));
  // Deterministic: the same picture gives the same file.
  const auto again = test::write_from("dicom", grey);
  REQUIRE_OK(again);
  CHECK(*again == *written);
  // Colour multi-frame.
  ir::Image colour;
  colour.frames.push_back(test::gradient_frame(6, 4));
  colour.frames.push_back(test::checker_frame(6, 4, 2));
  for (auto& f : colour.frames) {
    for (std::size_t i = 3; i < f.rgba.size(); i += 4) f.rgba[i] = 255;
  }
  const auto mf = test::write_from("dicom", colour);
  REQUIRE_OK(mf);
  const auto mf_back = test::read_as<ir::Image>("dicom", *mf);
  REQUIRE_OK(mf_back);
  CHECK(test::pixels_equal(*mf_back, colour));
}

TEST_CASE("science/dicom: hostile input") {
  for (const char* name : {"ct_explicit_le.dcm", "ct_implicit_le.dcm", "ct_deflated_undefined_seq.dcm", "mf_rle.dcm", "palette.dcm"}) {
    CHECK(test::fuzz_reader<ir::Image>("dicom", dcm(name), 256) > 0);
    CHECK(test::fuzz_reader<ir::Data>("dicom", dcm(name), 128) > 0);
  }
  // Deeply nested undefined-length sequences.
  std::string nested(128, '\0');
  nested += "DICM";
  std::string body;
  for (int i = 0; i < 200; ++i) body += std::string("\x08\x00\x40\x11SQ\0\0\xFF\xFF\xFF\xFF\xFE\xFF\x00\xE0\xFF\xFF\xFF\xFF", 20);
  CHECK(test::read_as<ir::Data>("dicom", as_bytes(nested + body)).error().code != Status::Ok);
}
