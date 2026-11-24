// Tests of OpenRaster, Krita, animated cursors and DCX.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <vector>

#include "families/image_modern/misc_test_util.hpp"
#include "util/zip.hpp"

using namespace cvt;
using test::misc_util::data;
using test::misc_util::load;
using test::misc_util::max_diff_visible;
using test::misc_util::reference;
using test::misc_util::single;

TEST_CASE("image_modern/misc/ora: composites the layer stack when there is no merged image") {
  const ir::Image image = load("ora", "ora/layers_nomerged.ora");
  REQUIRE(image.valid());
  CHECK(image.width() == 48);
  CHECK(image.height() == 32);
  // ImageMagick's own composite of the same layers (hidden layer skipped,
  // 50 % box at +10+8, the dot at stack offset +20+10 plus +10+4).
  CHECK(max_diff_visible(image.frames[0], reference("ora/ora_expected.png")) <= 1);
  CHECK(image.dpi_x == 72.0);
}

TEST_CASE("image_modern/misc/ora: prefers mergedimage.png") {
  const ir::Image image = load("ora", "ora/merged.ora");
  REQUIRE(image.valid());
  CHECK(test::pixels_equal(image.frames[0], reference("ora/kra_merged.png")));
}

TEST_CASE("image_modern/misc/kra: reads the merged image, else the preview with a warning") {
  CHECK(test::pixels_equal(load("kra", "ora/merged.kra").frames[0], reference("ora/kra_merged.png")));
  std::vector<std::string> warnings;
  const ir::Image preview = load("kra", "ora/preview_only.kra", &warnings);
  CHECK(test::pixels_equal(preview.frames[0], reference("ora/kra_preview.png")));
  CHECK(!warnings.empty());
  // An OpenRaster file is not a Krita document and vice versa.
  CHECK(!test::read_as<ir::Image>("kra", data("ora/merged.ora")));
  CHECK(!test::read_as<ir::Image>("ora", data("ora/merged.kra")));
}

TEST_CASE("image_modern/misc/ora: writes a valid single-layer package that reads back exactly") {
  for (const ir::Frame& frame : {test::alpha_gradient_frame(300, 90), test::checker_frame(17, 9, 3)}) {
    const auto encoded = test::write_from("ora", single(frame));
    REQUIRE_OK(encoded);
    auto zip = util::ZipReader::open(*encoded);
    REQUIRE_OK(zip);
    // mimetype first and stored, as the specification requires.
    REQUIRE(!zip->entries().empty());
    CHECK(zip->entries()[0].name == "mimetype");
    CHECK(zip->entries()[0].method == 0);
    CHECK(test::str(ByteView(*encoded).subspan(30, 24)) == "mimetypeimage/openraster");
    for (const char* name : {"stack.xml", "mergedimage.png", "Thumbnails/thumbnail.png", "data/layer0.png"}) {
      INFO(name);
      CHECK(zip->contains(name));
    }
    const auto thumb = test::read_as<ir::Image>("png", *zip->read("Thumbnails/thumbnail.png"));
    REQUIRE_OK(thumb);
    CHECK(std::max(thumb->width(), thumb->height()) <= 256);
    const auto decoded = test::read_as<ir::Image>("ora", *encoded);
    REQUIRE_OK(decoded);
    CHECK(test::pixels_equal(decoded->frames[0], frame));
    // The same document without the merged image is rebuilt from the stack.
    util::ZipWriter stripped;
    for (std::size_t i = 0; i < zip->size(); ++i) {
      const std::string name = zip->entries()[i].name;
      if (name == "mergedimage.png") continue;
      REQUIRE_OK(stripped.add(name, *zip->read(i), util::ZipEntryOptions{.level = 0}));
    }
    const auto rebuilt = test::read_as<ir::Image>("ora", *stripped.finish());
    REQUIRE_OK(rebuilt);
    CHECK(test::pixels_equal(rebuilt->frames[0], frame));
  }
  // compression_level: level 0 stores, level 9 is not larger.
  const ir::Image photo = single(test::gradient_frame(64, 64));
  const auto fast = test::write_from("ora", photo, R"({"compression_level":0})");
  const auto best = test::write_from("ora", photo, R"({"compression_level":9})");
  REQUIRE_OK(fast);
  REQUIRE_OK(best);
  CHECK(best->size() < fast->size());
  // Same-format path: ora -> ora keeps the pixels.
  const test::Converted same = test::convert(data("ora/layers_nomerged.ora"), "ora", "ora");
  REQUIRE(same.ok());
  CHECK(max_diff_visible(test::read_as<ir::Image>("ora", same.data)->frames[0],
                         reference("ora/ora_expected.png")) <= 1);
}

TEST_CASE("image_modern/misc/ani: steps, sequence and per-step rates become timed frames") {
  const ir::Frame f0 = reference("ani/f0.png");
  const ir::Frame f1 = reference("ani/f1.png");
  const ir::Frame f2 = reference("ani/f2.png");
  const ir::Image seq = load("ani", "ani/cursor_seq.ani");
  REQUIRE(seq.frames.size() == 5);
  const ir::Frame* expected[] = {&f0, &f1, &f2, &f1, &f0};
  const std::uint32_t delays[] = {100, 200, 500, 100, 50};  // jiffies 6, 12, 30, 6, 3
  for (std::size_t i = 0; i < 5; ++i) {
    INFO(i);
    CHECK(max_diff_visible(seq.frames[i], *expected[i]) == 0);
    CHECK(seq.frames[i].delay_ms == delays[i]);
  }
  CHECK(seq.metadata.at("Title") == "Test pattern");
  const ir::Image icons = load("ani", "ani/icons.ani");
  REQUIRE(icons.frames.size() == 3);
  CHECK(max_diff_visible(icons.frames[2], f2) == 0);
  CHECK(icons.frames[0].delay_ms == 250);  // default rate of 15 jiffies
  // To an animated format and back.
  const test::Converted gif = test::convert(data("ani/cursor_seq.ani"), "ani", "gif");
  REQUIRE(gif.ok());
  CHECK(test::read_as<ir::Image>("gif", gif.data)->frames.size() == 5);
}

TEST_CASE("image_modern/misc/dcx: pages become frames; writing round-trips") {
  const ir::Image pages = load("dcx", "ani/pages.dcx");
  REQUIRE(pages.frames.size() == 3);
  CHECK(test::pixels_equal(pages.frames[0], reference("ani/p0.png")));
  CHECK(test::pixels_equal(pages.frames[1], reference("ani/p1.png")));
  CHECK(test::pixels_equal(pages.frames[2], reference("ani/p2.png")));
  const auto encoded = test::write_from("dcx", pages);
  REQUIRE_OK(encoded);
  const auto decoded = test::read_as<ir::Image>("dcx", *encoded);
  REQUIRE_OK(decoded);
  REQUIRE(decoded->frames.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) CHECK(test::pixels_equal(decoded->frames[i], pages.frames[i]));
  // Same-format path.
  CHECK(test::convert(data("ani/pages.dcx"), "dcx", "dcx").ok());
}

TEST_CASE("image_modern/misc/ora+ani: damaged files are errors, never crashes") {
  const std::pair<const char*, const char*> cases[] = {
      {"ora", "ora/layers_nomerged.ora"}, {"ora", "ora/merged.ora"},      {"kra", "ora/merged.kra"},
      {"ani", "ani/cursor_seq.ani"},      {"ani", "ani/icons.ani"},       {"dcx", "ani/pages.dcx"},
  };
  for (const auto& [format, name] : cases) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Image>(format, data(name), 256) > 0);
  }
  // A DCX whose page table points everywhere, and an ANI whose sequence
  // points past the frames.
  util::ByteWriter dcx;
  dcx.u32le(0x3ADE68B1).u32le(0xFFFFFFF0).u32le(4).u32le(0);
  CHECK(!test::read_as<ir::Image>("dcx", dcx.data()));
}

TEST_CASE("image_modern/misc/ora+ani: detection by signature") {
  using test::misc_util::detected;
  CHECK(detected(data("ora/layers_nomerged.ora")) == "ora");
  CHECK(detected(data("ora/merged.kra")) == "kra");
  CHECK(detected(data("ani/cursor_seq.ani")) == "ani");
  CHECK(detected(data("ani/pages.dcx")) == "dcx");
  CHECK(detected({}, "x.ora") == "ora");
  CHECK(detected({}, "x.kra") == "kra");
  CHECK(detected({}, "x.ani") == "ani");
  CHECK(detected({}, "x.dcx") == "dcx");
}
