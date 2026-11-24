// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// pdf family: CCITT fax and JBIG2 decoding against independent encoders.
#include <string>

#include "families/pdf/bilevel.hpp"
#include "families/pdf/pdf_test_util.hpp"

using namespace cvt;

TEST_CASE("pdf/ccitt: G4, G3 1-D and G3 2-D decode exactly") {
  const pdf_test::Pbm reference = pdf_test::load_pbm("bilevel/fax.pbm");
  REQUIRE(reference.width == 200);
  REQUIRE(reference.height == 80);
  struct Case {
    const char* file;
    int k;
  };
  for (const Case c : {Case{"bilevel/fax_g4.raw", -1}, Case{"bilevel/fax_g3_1d.raw", 0}, Case{"bilevel/fax_g3_2d.raw", 1}}) {
    CAPTURE(c.file);
    pdf::bilevel::CcittParams p;
    p.k = c.k;
    p.columns = 200;
    p.rows = 80;
    p.black_is_1 = true;
    const auto decoded = pdf::bilevel::ccitt_decode(test::fixture(std::string("pdf/") + c.file), p, 1 << 20);
    REQUIRE_OK(decoded);
    CHECK(pdf_test::unpack(*decoded, 200, 80, true) == reference.px);
  }
  // Through the PDF filter: tiff2pdf's G4 image (BlackIs1 true).
  const Bytes pdf_bytes = test::fixture("pdf/ccitt_g4.pdf");
  auto file = pdf::PdfFile::open(pdf_bytes);
  REQUIRE(file.has_value());
  const pdf::Object xobjects = (*file)->get((*file)->pages()[0].resources.as_dict(), "XObject");
  REQUIRE(xobjects.as_dict().size() == 1);
  auto image = (*file)->decode(xobjects.as_dict().begin()->second);
  REQUIRE_OK(image);
  CHECK(pdf_test::unpack(image->data, 200, 80, true) == reference.px);
}

TEST_CASE("pdf/ccitt: damaged data fails cleanly") {
  const Bytes g4 = test::fixture("pdf/bilevel/fax_g4.raw");
  pdf::bilevel::CcittParams p;
  p.k = -1;
  p.columns = 200;
  p.rows = 80;
  for (std::size_t len = 0; len < g4.size(); len += 7) (void)pdf::bilevel::ccitt_decode(ByteView(g4).first(len), p, 1 << 20);
  for (std::uint32_t seed = 1; seed < 40; ++seed) (void)pdf::bilevel::ccitt_decode(test::random_bytes(300, seed), p, 1 << 20);
  p.rows = 0;
  p.columns = 1 << 20;
  CHECK(!pdf::bilevel::ccitt_decode(test::random_bytes(64, 3), p, 4096).has_value());
}

TEST_CASE("pdf/jbig2: generic regions (MQ coder, TPGDON) and text regions decode exactly") {
  const pdf_test::Pbm fax = pdf_test::load_pbm("bilevel/fax.pbm");
  for (const char* name : {"bilevel/fax_generic.jb2", "bilevel/fax_tpgd.jb2"}) {
    CAPTURE(name);
    std::uint32_t w = 0, h = 0;
    const auto decoded = pdf::bilevel::jbig2_decode(test::fixture(std::string("pdf/") + name), {}, 1 << 24, &w, &h);
    REQUIRE_OK(decoded);
    CHECK(w == 200);
    CHECK(h == 80);
    CHECK(pdf_test::unpack(*decoded, w, h, false) == fax.px);
  }
  const pdf_test::Pbm text = pdf_test::load_pbm("bilevel/jbig2_text.pbm");
  std::uint32_t w = 0, h = 0;
  const auto decoded = pdf::bilevel::jbig2_decode(test::fixture("pdf/bilevel/text_page.jb2"),
                                                  test::fixture("pdf/bilevel/text_symbols.jb2g"), 1 << 24, &w, &h);
  REQUIRE_OK(decoded);
  CHECK(w == text.width);
  CHECK(h == text.height);
  CHECK(pdf_test::unpack(*decoded, w, h, false) == text.px);
}

TEST_CASE("pdf/jbig2: hostile input fails cleanly") {
  const Bytes generic = test::fixture("pdf/bilevel/fax_generic.jb2");
  for (std::size_t len = 0; len < generic.size(); ++len) (void)pdf::bilevel::jbig2_decode(ByteView(generic).first(len), {}, 1 << 24);
  for (std::uint32_t seed = 1; seed < 64; ++seed) {
    Bytes mutated = generic;
    mutated[seed % mutated.size()] ^= std::byte{0x5A};
    mutated[(seed * 31) % mutated.size()] ^= std::byte{0x11};
    (void)pdf::bilevel::jbig2_decode(mutated, {}, 1 << 22);
  }
  // A page that claims to be enormous.
  Bytes page = generic;
  page[11] = std::byte{0x7F};  // width high byte of the page information segment
  CHECK(!pdf::bilevel::jbig2_decode(page, {}, 1 << 20).has_value());
}
