// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// pdf family: XPS / OpenXPS and FDF / XFDF readers.
//
// No independent XPS producer runs on the development machines, so the XPS
// package is assembled here by hand from the XPS specification (fixed page
// markup, an obfuscated copy of the bundled Inter font).
#include <string>

#include "families/pdf/embedded_fonts.hpp"
#include "families/pdf/pdf_test_util.hpp"
#include "ir/data.hpp"
#include "util/zip.hpp"

using namespace cvt;
using namespace std::string_view_literals;

namespace {

Bytes make_xps() {
  util::ZipWriter zip;
  (void)zip.add("[Content_Types].xml",
                R"(<?xml version="1.0" encoding="utf-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
                R"(<Default Extension="fdseq" ContentType="application/vnd.ms-package.xps-fixeddocumentsequence+xml"/>)"
                R"(<Default Extension="fdoc" ContentType="application/vnd.ms-package.xps-fixeddocument+xml"/>)"
                R"(<Default Extension="fpage" ContentType="application/vnd.ms-package.xps-fixedpage+xml"/>)"
                R"(<Default Extension="odttf" ContentType="application/vnd.ms-package.obfuscated-opentype"/></Types>)");
  (void)zip.add("_rels/.rels",
                R"(<?xml version="1.0" encoding="utf-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)"
                R"(<Relationship Type="http://schemas.microsoft.com/xps/2005/06/fixedrepresentation" Target="/FixedDocumentSequence.fdseq" Id="R0"/></Relationships>)");
  (void)zip.add("FixedDocumentSequence.fdseq",
                R"(<FixedDocumentSequence xmlns="http://schemas.microsoft.com/xps/2005/06"><DocumentReference Source="Documents/1/FixedDocument.fdoc"/></FixedDocumentSequence>)");
  (void)zip.add("Documents/1/FixedDocument.fdoc",
                R"(<FixedDocument xmlns="http://schemas.microsoft.com/xps/2005/06"><PageContent Source="Pages/1.fpage"/></FixedDocument>)");
  // Obfuscated font: XOR the first 32 bytes with the GUID of its name.
  const std::string guid = "0123456789ABCDEF0123456789ABCDEF";
  Bytes font(pdf::fonts::inter_regular().begin(), pdf::fonts::inter_regular().end());
  for (int k = 0; k < 16; ++k) {
    const auto key = static_cast<std::uint8_t>(std::stoi(guid.substr(static_cast<std::size_t>(2 * (15 - k)), 2), nullptr, 16));
    font[static_cast<std::size_t>(k)] ^= static_cast<std::byte>(key);
    font[static_cast<std::size_t>(k) + 16] ^= static_cast<std::byte>(key);
  }
  (void)zip.add("Resources/" + guid.substr(0, 8) + "-" + guid.substr(8, 4) + "-" + guid.substr(12, 4) + "-" + guid.substr(16, 4) +
                    "-" + guid.substr(20) + ".odttf",
                font);
  const std::string font_uri = "/Resources/" + guid.substr(0, 8) + "-" + guid.substr(8, 4) + "-" + guid.substr(12, 4) + "-" +
                               guid.substr(16, 4) + "-" + guid.substr(20) + ".odttf";
  (void)zip.add("Documents/1/Pages/1.fpage",
                R"(<FixedPage xmlns="http://schemas.microsoft.com/xps/2005/06" Width="400" Height="200">)"
                R"(<Path Data="M 20,20 L 120,20 L 120,80 L 20,80 Z" Fill="#FFFF0000"/>)"
                R"(<Canvas RenderTransform="1,0,0,1,0,100" Opacity="0.5"><Path Data="M 200,0 h 100 v 50 h -100 z" Fill="#0000FF" Stroke="#000000" StrokeThickness="2"/></Canvas>)"
                R"(<Glyphs FontUri=")" + font_uri +
                    R"(" FontRenderingEmSize="24" OriginX="20" OriginY="150" Fill="#FF000000" UnicodeString="Hello XPS page"/>)"
                    R"(</FixedPage>)");
  return *zip.finish();
}

}  // namespace

TEST_CASE("xps/read: text, shapes and glyphs of a fixed page") {
  const Bytes xps = make_xps();
  const test::Converted detected = test::convert(xps, "", "pdf", "", "sample.xps");
  CHECK(detected.ok());
  auto doc = test::read_as<ir::Document>("xps", xps);
  REQUIRE_OK(doc);
  CHECK(ir::plain_text(*doc) == "Hello XPS page\n");
  auto image = test::read_as<ir::Image>("xps", xps, R"({"dpi":96})");
  REQUIRE_OK(image);
  const ir::Frame& f = image->frames[0];
  REQUIRE(f.width == 400);
  REQUIRE(f.height == 200);
  const auto px = [&](std::uint32_t x, std::uint32_t y) {
    const std::size_t o = f.offset(x, y);
    return std::array<int, 3>{f.rgba[o], f.rgba[o + 1], f.rgba[o + 2]};
  };
  CHECK(px(60, 50) == std::array<int, 3>{255, 0, 0});
  const auto half_blue = px(250, 125);
  CHECK(std::abs(half_blue[2] - 255) <= 1);
  CHECK(std::abs(half_blue[0] - 128) <= 2);
  // Some ink where the text is.
  int dark = 0;
  for (std::uint32_t x = 20; x < 200; ++x) dark += px(x, 145)[0] < 100 ? 1 : 0;
  CHECK(dark > 10);
  CHECK(test::fuzz_reader<ir::Document>("xps", xps, 24) > 0);
  CHECK_STATUS(test::read_as<ir::Document>("xps", as_bytes("PK\x03\x04garbage"sv)), Status::Decode);
}

TEST_CASE("fdf/read: field values with nesting and arrays") {
  const std::string fdf =
      "%FDF-1.2\n1 0 obj\n<< /FDF << /F (form.pdf) /Fields [ << /T (name) /V (Ada \\(test\\)) >> "
      "<< /T (address) /Kids [ << /T (city) /V (Z\\374rich) >> << /T (zip) /V 8001 >> ] >> "
      "<< /T (agree) /V /Yes >> << /T (choices) /V [(a) (b)] >> ] >> >>\nendobj\n"
      "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
  auto data = test::read_as<ir::Data>("fdf", as_bytes(fdf));
  REQUIRE_OK(data);
  const Json expected = Json::object({{"file", "form.pdf"},
                                      {"fields", Json::object({{"name", "Ada (test)"},
                                                               {"address.city", "Z\xC3\xBCrich"},
                                                               {"address.zip", 8001},
                                                               {"agree", "Yes"},
                                                               {"choices", Json::array({"a", "b"})}})}});
  CHECK_MESSAGE(test::data_equal(data->value, expected), test::data_diff(data->value, expected));
  CHECK(test::convert(as_bytes(fdf), "", "json").ok() || test::find_writer("json", Kind::Data) == nullptr);
  CHECK(test::fuzz_reader<ir::Data>("fdf", as_bytes(fdf), 64) > 0);
}

TEST_CASE("xfdf/read: field values") {
  const std::string xfdf =
      R"(<?xml version="1.0" encoding="UTF-8"?><xfdf xmlns="http://ns.adobe.com/xfdf/" xml:space="preserve">)"
      R"(<f href="form.pdf"/><fields><field name="name"><value>Ada</value></field>)"
      R"(<field name="address"><field name="city"><value>Bern</value></field></field>)"
      R"(<field name="choices"><value>a</value><value>b</value></field></fields></xfdf>)";
  auto data = test::read_as<ir::Data>("xfdf", as_bytes(xfdf));
  REQUIRE_OK(data);
  const Json expected = Json::object(
      {{"file", "form.pdf"},
       {"fields", Json::object({{"name", "Ada"}, {"address.city", "Bern"}, {"choices", Json::array({"a", "b"})}})}});
  CHECK_MESSAGE(test::data_equal(data->value, expected), test::data_diff(data->value, expected));
  CHECK(test::fuzz_reader<ir::Data>("xfdf", as_bytes(xfdf), 64) > 0);
}
