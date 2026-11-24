// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// pdf family: rendering pages and comparing with Ghostscript references.
#include <string>

#include "families/pdf/pdf_test_util.hpp"

using namespace cvt;

namespace {

ir::Frame render(std::string_view name, std::string_view options = R"({"dpi":36})") {
  const Bytes data = test::fixture(std::string("pdf/") + std::string(name));
  auto image = test::read_as<ir::Image>("pdf", data, options);
  REQUIRE_OK(image);
  return image->frames.front();
}

}  // namespace

TEST_CASE("pdf/render: pages match Ghostscript references") {
  struct Case {
    const char* name;
    double min_ssim;
    double min_psnr;
  };
  // At 36 dpi text is a few pixels tall; thresholds leave room for different
  // anti-aliasing, hinting and (for non-embedded fonts) substitute faces.
  const Case cases[] = {
      {"gs_graphics", 0.95, 23},      {"latex_article", 0.85, 21},       {"gs_cff_article", 0.85, 21},
      {"libreoffice_report", 0.94, 24}, {"weasyprint_report", 0.92, 21}, {"gs_type3_base14", 0.92, 21},
      {"latex_twocolumn", 0.82, 19},   {"img2pdf_png", 0.93, 23},         {"ccitt_g4", 0.85, 13},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    const ir::Frame ours = pdf_test::to_gray(render(std::string(c.name) + ".pdf"));
    const ir::Frame reference = pdf_test::load_gray_reference(c.name);
    REQUIRE(reference.valid());
    CHECK(ours.width == reference.width);
    CHECK(ours.height == reference.height);
    CHECK(test::ssim(ours, reference) >= c.min_ssim);
    CHECK(test::psnr(ours, reference) >= c.min_psnr);
  }
}

TEST_CASE("pdf/render: known pixels of the graphics test page") {
  const ir::Frame f = render("gs_graphics.pdf", R"({"dpi":72})");
  REQUIRE(f.width == 300);
  REQUIRE(f.height == 300);
  const auto px = [&](int x, int y) {
    const std::size_t o = f.offset(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    return std::array<int, 3>{f.rgba[o], f.rgba[o + 1], f.rgba[o + 2]};
  };
  // Red rectangle 20..100 x 220..280 (PDF y up) -> device y 20..80.
  CHECK(px(60, 50) == std::array<int, 3>{255, 0, 0});
  // CMYK 0 1 1 0 square at 250..280 -> red as well.
  CHECK(px(265, 65) == std::array<int, 3>{255, 0, 0});
  // Background 0.95 gray.
  CHECK(std::abs(px(5, 5)[0] - 242) <= 1);
  // Centre of the even-odd star is a hole (background), its points are blue.
  CHECK(std::abs(px(190, 63)[0] - 242) <= 2);
  // Axial shading: yellow at the left end, bluish at the right end.
  const auto left = px(142, 260), right = px(278, 260);
  CHECK(left[0] > 230);
  CHECK(left[2] < 40);
  CHECK(right[2] > 230);
  CHECK(right[0] < 30);
}

TEST_CASE("pdf/render: options (dpi, pages, background) and multi-page output") {
  const Bytes data = test::fixture("pdf/gs_two_pages.pdf");
  auto both = test::read_as<ir::Image>("pdf", data, R"({"dpi":30})");
  REQUIRE_OK(both);
  CHECK(both->frames.size() == 2);
  auto second = test::read_as<ir::Image>("pdf", data, R"({"dpi":30,"pages":"2"})");
  REQUIRE_OK(second);
  CHECK(second->frames.size() == 1);
  CHECK(second->frames[0].width > second->frames[0].height);  // landscape page
  auto transparent = test::read_as<ir::Image>("pdf", test::fixture("pdf/latex_small.pdf"), R"({"dpi":20,"background":"transparent"})");
  REQUIRE_OK(transparent);
  CHECK(transparent->frames[0].rgba[3] == 0);
  auto colored = test::read_as<ir::Image>("pdf", test::fixture("pdf/latex_small.pdf"), R"({"dpi":20,"background":"#102030"})");
  REQUIRE_OK(colored);
  CHECK(colored->frames[0].rgba[0] == 0x10);
  CHECK_STATUS(test::read_as<ir::Image>("pdf", data, R"({"background":"purple-ish"})"), Status::InvalidArgument);
  const ir::Frame low = render("latex_small.pdf", R"({"dpi":36})");
  const ir::Frame high = render("latex_small.pdf", R"({"dpi":72})");
  CHECK(high.width == low.width * 2);
  Limits tiny;
  tiny.max_pixels = 1000;
  CHECK_STATUS(test::read_as<ir::Image>("pdf", data, R"({"dpi":300})", nullptr, tiny), Status::Limit);
}

TEST_CASE("pdf/render: encrypted files render like their plain source") {
  const ir::Frame plain = render("latex_small.pdf");
  for (const char* name : {"enc_rc4_40.pdf", "enc_aes_256.pdf"}) {
    CAPTURE(name);
    CHECK(test::pixels_equal(render(name), plain));
  }
  const ir::Frame user = render("enc_aes_256_user.pdf", R"({"dpi":36,"password":"secret"})");
  CHECK(test::pixels_equal(user, plain));
}

TEST_CASE("pdf/render: JPEG images decode through the registry when available") {
  const Bytes data = test::fixture("pdf/img2pdf_jpeg.pdf");
  std::vector<std::string> warnings;
  auto image = test::read_as<ir::Image>("pdf", data, R"({"dpi":72})", &warnings);
  REQUIRE_OK(image);
  if (test::find_reader("jpg", Kind::Image) == nullptr) {
    CHECK(!warnings.empty());  // the picture is left blank, with a warning
    return;
  }
  auto direct = test::read_as<ir::Image>("jpg", test::fixture("pdf/photo.jpg"));
  REQUIRE_OK(direct);
  CHECK(test::psnr(image->frames[0], direct->frames[0]) >= 30);
}

TEST_CASE("pdf/render: the image converts through the engine to every image writer") {
  const Bytes data = test::fixture("pdf/latex_small.pdf");
  for (const char* target : {"png", "jpg", "tiff", "bmp"}) {
    if (test::find_writer(target, Kind::Image) == nullptr) continue;
    CAPTURE(target);
    CHECK(test::convert(data, "pdf", target, R"({"dpi":20})").ok());
  }
}
