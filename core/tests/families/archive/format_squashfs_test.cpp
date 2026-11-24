// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// SquashFS 4.0 images made by mksquashfs with every compressor.
#include <algorithm>

#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/compression.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {
Bytes sqfs(std::string_view name) { return test::fixture(std::string("archive/squashfs/") + std::string(name)); }

ir::Archive read_ok(std::string_view name) {
  INFO("fixture ", name);
  auto archive = test::read_as<ir::Archive>("squashfs", sqfs(name));
  REQUIRE_OK(archive);
  return std::move(*archive);
}
}  // namespace

TEST_CASE("archive/squashfs: images from mksquashfs extract exactly with every compressor") {
  struct Case {
    const char* fixture;
    compression::Method method;
  };
  for (const Case& c : {Case{"tree_gzip.sqfs", compression::Method::Zlib}, Case{"tree_xz.sqfs", compression::Method::Xz},
                        Case{"tree_lzma.sqfs", compression::Method::LzmaAlone},
                        Case{"tree_lz4.sqfs", compression::Method::Lz4Block},
                        Case{"tree_zstd.sqfs", compression::Method::Zstd}, Case{"tree_lzo.sqfs", compression::Method::Lzo1x},
                        Case{"tree_uncompressed.sqfs", compression::Method::Store}}) {
    INFO("fixture ", c.fixture);
    if (!compression::can_decode(c.method)) {
      MESSAGE("skipping ", c.fixture, ": ", compression::method_name(c.method), " not available yet");
      continue;
    }
    const ir::Archive archive = read_ok(c.fixture);
    tree::check(archive);
    // Directories carry their mode and time too.
    for (const auto& e : archive.entries) {
      if (e.is_directory) {
        CHECK_EQ(e.unix_mode, 0755u);
        CHECK_EQ(e.mtime, tree::mtime);
      }
    }
  }
}

TEST_CASE("archive/squashfs: multi-block files and sparse blocks") {
  const ir::Archive archive = read_ok("sparse_gzip.sqfs");
  REQUIRE_EQ(archive.entries.size(), 2u);
  std::string lorem;
  for (int i = 0; i < 400; ++i) lorem += tree::lorem_paragraph();
  bool saw_big = false;
  bool saw_sparse = false;
  for (const auto& e : archive.entries) {
    if (e.path == "big.txt") {
      saw_big = true;
      CHECK(test::str(e.bytes) == lorem);
    } else if (e.path == "sparse.bin") {
      saw_sparse = true;
      REQUIRE_EQ(e.bytes.size(), 300003u);
      CHECK(test::str(ByteView(e.bytes).first(5)) == "begin");
      CHECK(test::str(ByteView(e.bytes).last(3)) == "end");
      CHECK(std::all_of(e.bytes.begin() + 5, e.bytes.end() - 3, [](std::byte b) { return b == std::byte{0}; }));
    }
    CHECK_EQ(e.mtime, tree::mtime);
  }
  CHECK(saw_big);
  CHECK(saw_sparse);
}

TEST_CASE("archive/squashfs: damaged and hostile images fail cleanly") {
  test::fuzz_reader<ir::Archive>("squashfs", sqfs("tree_gzip.sqfs"));
  test::fuzz_reader<ir::Archive>("squashfs", sqfs("tree_uncompressed.sqfs"));
  test::fuzz_reader<ir::Archive>("squashfs", sqfs("sparse_gzip.sqfs"));
  CHECK_STATUS(test::read_as<ir::Archive>("squashfs", test::random_bytes(4096, 3)), Status::Decode);
  // Every truncation fails (the superblock records the image size).
  const Bytes image = sqfs("tree_gzip.sqfs");
  for (std::size_t n = 0; n < 4096; n += 97) {
    CHECK_FALSE(test::read_as<ir::Archive>("squashfs", ByteView(image).first(n)).has_value());
  }
  // Limits: entry count and decompressed size.
  Limits few;
  few.max_archive_entries = 4;
  CHECK_STATUS(test::read_as<ir::Archive>("squashfs", image, "", nullptr, few), Status::Limit);
  Limits small;
  small.max_decompressed_bytes = 100000;
  CHECK_STATUS(test::read_as<ir::Archive>("squashfs", sqfs("sparse_gzip.sqfs"), "", nullptr, small), Status::Limit);
  // A directory entry that points back at the root inode is refused (no endless walk).
  Bytes cyclic = sqfs("tree_uncompressed.sqfs");
  const auto u16 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(cyclic[at]) | (std::to_integer<unsigned>(cyclic[at + 1]) << 8));
  };
  const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
  const std::uint32_t root_block = u32(34) & 0xFFFFu;  // root_inode_ref >> 16 (small image)
  const std::uint32_t root_offset = u32(32) & 0xFFFFu;
  const std::size_t inode_table = u32(64);
  const std::size_t dir_table = u32(72);
  const std::size_t root = inode_table + root_block + 2 + root_offset;
  REQUIRE_EQ(u16(root), 1u);  // basic directory
  const std::size_t listing = dir_table + u32(root + 16) + 2 + u16(root + 26);
  REQUIRE_EQ(u32(listing + 4), root_block);  // entries live in the root's inode block
  bool patched = false;
  for (std::size_t p = listing + 12, n = 0; n <= u32(listing) && !patched; ++n) {
    if (u16(p + 4) == 1) {  // first sub-directory: point it at the root
      cyclic[p] = static_cast<std::byte>(root_offset & 0xFFu);
      cyclic[p + 1] = static_cast<std::byte>(root_offset >> 8);
      patched = true;
    }
    p += 8 + u16(p + 6) + 1;
  }
  REQUIRE(patched);
  CHECK_STATUS(test::read_as<ir::Archive>("squashfs", cyclic), Status::Decode);
  // Unknown compressor id.
  Bytes odd = image;
  odd[20] = std::byte{9};
  CHECK_STATUS(test::read_as<ir::Archive>("squashfs", odd), Status::Decode);
  // Version 3 images are refused with a precise message.
  odd = image;
  odd[28] = std::byte{3};
  const auto v3 = test::read_as<ir::Archive>("squashfs", odd);
  REQUIRE_FALSE(v3.has_value());
  CHECK(v3.error().message.find("4.0") != std::string::npos);
}

TEST_CASE("archive/squashfs: detection and conversion to tar") {
  const Bytes image = sqfs("tree_gzip.sqfs");
  const auto detected = detect_format(Registry::instance(), image, "");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "squashfs");
  CHECK(detect_format(Registry::instance(), image, "rootfs.squashfs")->format->id == "squashfs");
  const test::Converted out = test::convert(image, "", "tar");
  REQUIRE(out.ok());
  const auto back = test::read_as<ir::Archive>("tar", out.data);
  REQUIRE_OK(back);
  tree::check(*back);
}
