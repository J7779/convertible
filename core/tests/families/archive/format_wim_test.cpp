// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Windows Imaging (WIM / ESD) archives from wimlib.
#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/compression.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {
// wimcapture without --unix-data: no permissions, links as reparse points.
const tree::Check no_modes{.modes = false};
}  // namespace

TEST_CASE("archive/wim: uncompressed image with UNIX data extracts exactly") {
  const Bytes wim = test::fixture("archive/wim/tree_none.wim");
  const auto archive = test::read_as<ir::Archive>("wim", wim);
  REQUIRE_OK(archive);
  tree::check(*archive);
  CHECK(archive->entries.size() == 15u);
  const auto d = detect_format(Registry::instance(), wim, "");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "wim");
  // Only image 1 exists.
  CHECK_STATUS(test::read_as<ir::Archive>("wim", wim, R"({"wim_image":2})"), Status::InvalidArgument);
  REQUIRE_OK(test::read_as<ir::Archive>("wim", wim, R"({"wim_image":1})"));
}

TEST_CASE("archive/wim: XPRESS, LZX and LZMS images, solid or not") {
  struct Case {
    const char* fixture;
    compression::Method method;
  };
  for (const Case& c : {Case{"wim/tree_fast.wim", compression::Method::XpressHuffman},
                        Case{"wim/tree_maximum.wim", compression::Method::LzxWim},
                        Case{"wim/tree_lzms.wim", compression::Method::Lzms},
                        Case{"wim/tree_solid.wim", compression::Method::Lzms}}) {
    INFO("fixture ", c.fixture);
    const Bytes wim = test::fixture(std::string("archive/") + c.fixture);
    if (!compression::can_decode(c.method)) {
      MESSAGE("skipping ", c.fixture, ": ", compression::method_name(c.method), " not available yet");
      CHECK_STATUS(test::read_as<ir::Archive>("wim", wim), Status::Decode);
      continue;
    }
    const auto archive = test::read_as<ir::Archive>("wim", wim);
    REQUIRE_OK(archive);
    tree::check(*archive, no_modes);
    test::fuzz_reader<ir::Archive>("wim", wim, 128);
  }
}

TEST_CASE("archive/wim: damaged files, limits and conversion") {
  const Bytes wim = test::fixture("archive/wim/tree_none.wim");
  test::fuzz_reader<ir::Archive>("wim", wim);
  CHECK_STATUS(test::read_as<ir::Archive>("wim", test::random_bytes(4000, 9)), Status::Decode);
  // A flipped byte in file data fails the SHA-1 check.
  Bytes bad = wim;
  const std::string text = test::str(bad);
  const std::size_t at = text.find("Hello, archive!");
  REQUIRE(at != std::string::npos);
  bad[at] = std::byte{'J'};
  CHECK_STATUS(test::read_as<ir::Archive>("wim", bad), Status::Decode);
  // Pipable WIMs are reported precisely.
  Bytes pipable = wim;
  std::copy_n(as_bytes(std::string_view("WLPWM\0\0\0", 8)).begin(), 8, pipable.begin());
  const auto p = test::read_as<ir::Archive>("wim", pipable);
  REQUIRE_FALSE(p.has_value());
  CHECK(p.error().message.find("pipable") != std::string::npos);
  Limits few;
  few.max_archive_entries = 4;
  CHECK_STATUS(test::read_as<ir::Archive>("wim", wim, "", nullptr, few), Status::Limit);
  Limits small;
  small.max_decompressed_bytes = 4000;
  CHECK_STATUS(test::read_as<ir::Archive>("wim", wim, "", nullptr, small), Status::Limit);
  // wim -> tar keeps everything, including the symbolic link.
  const test::Converted tar = test::convert(wim, "", "tar");
  REQUIRE(tar.ok());
  tree::check(*test::read_as<ir::Archive>("tar", tar.data));
  CHECK(test::find_writer("wim", Kind::Archive) == nullptr);
}
