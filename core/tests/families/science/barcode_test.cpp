// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/science/barcode.hpp"
#include "families/science/science_test.hpp"

using namespace cvt;
namespace bc = cvt::science::barcode;

namespace {

std::vector<std::string> rows_of(const std::string& name) {
  std::vector<std::string> rows;
  std::string line;
  for (const char c : test::fixture_text("science/barcode/" + name)) {
    if (c == '\n') {
      rows.push_back(line);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  return rows;
}

std::vector<std::string> rows_of(const bc::Matrix& m) {
  std::vector<std::string> rows;
  for (int y = 0; y < m.height; ++y) {
    std::string r;
    for (int x = 0; x < m.width; ++x) r.push_back(m.get(x, y) ? '1' : '0');
    rows.push_back(r);
  }
  return rows;
}

std::string bits(const std::vector<bool>& b) {
  std::string s;
  for (const bool v : b) s.push_back(v ? '1' : '0');
  return s;
}

}  // namespace

TEST_CASE("science/qr: module matrices equal python-qrcode's for fixed version, level and mask") {
  struct Case {
    const char* text;
    int version;
    bc::QrEcc ecc;
    int mask;
    const char* file;
  };
  const Case cases[] = {{"HELLO WORLD", 1, bc::QrEcc::Q, 2, "qr_v1_q_m2.txt"},
                        {"01234567890123456789", 2, bc::QrEcc::M, 5, "qr_v2_m_m5.txt"},
                        {"Convertible test payload", 7, bc::QrEcc::H, 0, "qr_v7_h_m0.txt"},
                        {"byte mode: lower case", 10, bc::QrEcc::L, 6, "qr_v10_l_m6.txt"}};
  for (const Case& c : cases) {
    INFO(c.file);
    bc::QrOptions o;
    o.ecc = c.ecc;
    o.min_version = c.version;
    o.mask = c.mask;
    o.boost_ecc = false;
    int version = 0;
    const auto m = bc::qr_encode(c.text, o, &version);
    REQUIRE_OK(m);
    CHECK(version == c.version);
    CHECK(rows_of(*m) == rows_of(c.file));
  }
}

TEST_CASE("science/qr: capacity, versions and automatic mask") {
  bc::QrOptions o;
  o.ecc = bc::QrEcc::L;
  int version = 0, mask = -1;
  // Version 40-L holds 7089 digits, 4296 alphanumerics, 2953 bytes.
  CHECK_OK(bc::qr_encode(std::string(7089, '7'), o, &version));
  CHECK(version == 40);
  CHECK_STATUS(bc::qr_encode(std::string(7090, '7'), o), Status::Encode);
  CHECK_OK(bc::qr_encode(std::string(4296, 'A'), o));
  CHECK_OK(bc::qr_encode(std::string(2953, 'a'), o));
  CHECK_STATUS(bc::qr_encode(std::string(2954, 'a'), o), Status::Encode);
  const auto small = bc::qr_encode("HI", bc::QrOptions{}, &version, &mask);
  REQUIRE_OK(small);
  CHECK(version == 1);
  CHECK(small->width == 21);
  CHECK(mask >= 0);
  CHECK(mask <= 7);
}

TEST_CASE("science/datamatrix and linear symbols equal zxing-cpp's") {
  const auto dm = bc::datamatrix_encode("1234567890", false);
  REQUIRE_OK(dm);
  CHECK(rows_of(*dm) == rows_of("dm_digits.txt"));
  const auto dm40 = bc::datamatrix_encode("1234567890123456789012345678901234567890", false);
  REQUIRE_OK(dm40);
  CHECK(rows_of(*dm40) == rows_of("dm_digits40.txt"));
  const auto c128 = bc::code128_encode("Code128 test");
  REQUIRE_OK(c128);
  CHECK(bits(*c128) == rows_of("code128.txt").front());
  std::string full;
  const auto ean = bc::ean13_encode("590123412345", &full);
  REQUIRE_OK(ean);
  CHECK(full == "5901234123457");
  CHECK(bits(*ean) == rows_of("ean13.txt").front());
  const auto upc = bc::upca_encode("03600029145", &full);
  REQUIRE_OK(upc);
  CHECK(full == "036000291452");
  CHECK(bits(*upc) == rows_of("upca.txt").front());
  CHECK_STATUS(bc::ean13_encode("5901234123450"), Status::Encode);  // wrong check digit
  CHECK_STATUS(bc::code128_encode("\xC3\xA9"), Status::Encode);
  const auto rect = bc::datamatrix_encode("rect", true);
  REQUIRE_OK(rect);
  CHECK(rect->width != rect->height);
  CHECK_STATUS(bc::datamatrix_encode(std::string(3200, 'x'), false), Status::Encode);
}

TEST_CASE("science/barcodes: text converts to PNG, SVG and PBM; options apply") {
  const auto png = test::write_from("qr", ir::PlainText{"HELLO\n", "", false});
  REQUIRE_OK(png);
  CHECK(test::str(*png).substr(1, 3) == "PNG");
  const auto svg = test::write_from("qr", ir::PlainText{"HELLO", "", false}, R"({"barcode_output":"svg"})");
  REQUIRE_OK(svg);
  CHECK(test::str(*svg).starts_with("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"232\" height=\"232\""));  // (21+8)*8
  const auto pbm = test::write_from("code128", ir::PlainText{"AB", "", false}, R"({"barcode_output":"pbm","module_size":1,"bar_height":2})");
  REQUIRE_OK(pbm);
  CHECK(test::str(*pbm).starts_with("P4\n"));
  // Reading a text file as a symbology gives a picture with the module grid.
  const auto image = test::read_as<ir::Image>("datamatrix", as_bytes("1234567890\n"), R"({"module_size":2,"quiet_zone":1})");
  REQUIRE_OK(image);
  CHECK(image->width() == (12 + 2) * 2);
  CHECK(image->frames[0].rgba[image->frames[0].offset(2, 2)] == 0);    // top-left module of the L finder... dark
  CHECK(image->frames[0].rgba[image->frames[0].offset(0, 0)] == 255);  // quiet zone
  CHECK_STATUS(test::write_from("qr", ir::PlainText{"x", "", false}, R"({"foreground":"black"})"), Status::InvalidArgument);
  CHECK_STATUS(test::write_from("qr", ir::PlainText{std::string(500, 'x'), "", false}, R"({"qr_version":2})"), Status::Encode);
  std::vector<std::string> warnings;
  CHECK_OK(test::write_from("qr", ir::PlainText{"x", "", false}, R"({"qr_ecc":"H"})", &warnings));
  CHECK(warnings.size() == 1);  // the chosen version and mask
  // Through the engine from a .txt file (when the text reader is registered).
  if (Registry::instance().has_reader("txt")) {
    const auto converted = test::convert(std::string_view("hello from a text file\n"), "txt", "qr");
    CHECK(converted.ok());
  }
}
