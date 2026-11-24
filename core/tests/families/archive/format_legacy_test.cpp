// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The legacy DOS-era archives: LHA/LZH (jlha), ARJ (arj 3.10) and ARC (arc 5.21).
#include <algorithm>
#include <map>

#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/checksums.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/legacy_util.hpp"
#include "util/crc.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {
Bytes legacy_fixture(std::string_view name) { return test::fixture(std::string("archive/legacy/") + std::string(name)); }

ir::Archive read_ok(std::string_view format, std::string_view name) {
  INFO("fixture ", name);
  auto archive = test::read_as<ir::Archive>(format, legacy_fixture(name));
  REQUIRE_OK(archive);
  return std::move(*archive);
}

const ir::ArchiveEntry* find(const ir::Archive& a, std::string_view path) {
  for (const auto& e : a.entries) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

std::string detect_id(ByteView data, std::string_view name) {
  const auto d = detect_format(Registry::instance(), data, name);
  return d ? d->format->id : std::string();
}

// The ARC sample files made next to the tree (see legacy/SOURCES.md).
Bytes runs() {
  Bytes out;
  for (int i = 0; i < 200; ++i) out.insert(out.end(), static_cast<std::size_t>(20 + i % 13), static_cast<std::byte>(i % 7));
  return out;
}
}  // namespace

TEST_CASE("archive/lzh: stored entries at header levels 0, 1 and 2") {
  // jlha stores no Unix permissions, no symlinks and no unicode names here.
  const tree::Check lha{.symlink = false, .modes = false, .unicode = false};
  tree::check(read_ok("lzh", "tree_l1_lh0.lzh"), lha);
  tree::check(read_ok("lzh", "tree_l2_lh0.lzh"), lha);
  tree::Check level0 = lha;
  level0.long_name = false;  // level 0 cannot hold the long name
  tree::check(read_ok("lzh", "tree_l0_lh0.lzh"), level0);
  const ir::Archive a = read_ok("lzh", "tree_l2_lh0.lzh");
  const auto* dir = find(a, "dir/nested/deep");
  REQUIRE(dir != nullptr);
  CHECK(dir->is_directory);
  CHECK_EQ(dir->mtime, tree::mtime);
}

TEST_CASE("archive/lzh: compressed entries (-lh5-, -lh6-, -lh7-)") {
  if (!compression::can_decode(compression::Method::Lh)) {
    MESSAGE("skipping: the Lh codec is not available yet");
    // Without the codec the reader explains what is missing.
    const auto r = test::read_as<ir::Archive>("lzh", legacy_fixture("tree_l2_lh5.lzh"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("-lh5-") != std::string::npos);
    return;
  }
  const tree::Check lha{.symlink = false, .modes = false, .unicode = false};
  for (const char* name : {"tree_l2_lh5.lzh", "tree_l1_lh6.lzh", "tree_l2_lh7.lzh"}) {
    INFO("fixture ", name);
    tree::check(read_ok("lzh", name), lha);
  }
}

TEST_CASE("archive/arj: stored archive from arj 3.10 extracts exactly") {
  const ir::Archive a = read_ok("arj", "tree_m0.arj");
  // arj stores no directories and follows the symlink (a copy of hello.txt).
  tree::check(a, {.symlink = false, .empty_dir = false, .allow_extra = true});
  const auto* link = find(a, "link");
  REQUIRE(link != nullptr);
  CHECK(test::str(link->bytes) == "Hello, archive!\n");
  CHECK_EQ(a.entries.size(), 9u);
}

TEST_CASE("archive/arj: compressed methods 1 and 4") {
  if (!compression::can_decode(compression::Method::Arj)) {
    MESSAGE("skipping: the Arj codec is not available yet");
    const auto r = test::read_as<ir::Archive>("arj", legacy_fixture("tree_m1.arj"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("not available") != std::string::npos);
    return;
  }
  for (const char* name : {"tree_m1.arj", "tree_m4.arj"}) {
    INFO("fixture ", name);
    tree::check(read_ok("arj", name), {.symlink = false, .empty_dir = false, .allow_extra = true});
  }
}

TEST_CASE("archive/arc: stored, crunched, squeezed and squashed entries from arc 5.21") {
  const ir::Archive a = read_ok("arc", "flat.arc");
  REQUIRE_EQ(a.entries.size(), 6u);
  std::string lorem;
  for (int i = 0; i < 8; ++i) lorem += tree::lorem_paragraph();
  std::map<std::string, std::string> expected = {
      {"hello.txt", "Hello, archive!\n"}, {"lorem.txt", lorem}, {"noise.bin", test::str(tree::noise(4096))},
      {"empty.txt", ""}, {"runs.bin", test::str(runs())}};
  for (const auto& e : a.entries) {
    INFO("entry ", e.path);
    CHECK_EQ(e.mtime, tree::mtime);
    if (e.path == "skew.txt") {
      // Random letters from a skewed alphabet (Python's random module); the reader checked the CRC.
      CHECK_EQ(e.bytes.size(), 3000u);
      CHECK(std::all_of(e.bytes.begin(), e.bytes.end(), [](std::byte b) { return b >= std::byte{'a'} && b <= std::byte{'f'}; }));
      continue;
    }
    REQUIRE(expected.contains(e.path));
    CHECK(test::str(e.bytes) == expected[e.path]);
  }
  const ir::Archive squashed = read_ok("arc", "flat_squashed.arc");
  REQUIRE_EQ(squashed.entries.size(), 2u);
  CHECK(test::str(squashed.entries[0].bytes) == lorem);
}

TEST_CASE("archive/arc: the run-length and Huffman stages in isolation") {
  namespace lg = archive::legacy;
  // 'A' x 5, a literal 0x90, 'B'.
  const Bytes rle = {std::byte{'A'}, std::byte{0x90}, std::byte{5}, std::byte{0x90}, std::byte{0}, std::byte{'B'}};
  const auto plain = lg::arc_unrle(rle, 100);
  REQUIRE_OK(plain);
  CHECK(*plain == Bytes{std::byte{'A'}, std::byte{'A'}, std::byte{'A'}, std::byte{'A'}, std::byte{'A'}, std::byte{0x90}, std::byte{'B'}});
  CHECK_STATUS(lg::arc_unrle(rle, 6), Status::Decode);  // longer than declared
  CHECK_STATUS(lg::arc_unrle(Bytes{std::byte{0x90}, std::byte{9}}, 100), Status::Decode);
  // A packed (method 3) entry built by hand, with its CRC.
  Bytes arc = {std::byte{0x1A}, std::byte{3}};
  std::string name = "RUNS.TXT";
  name.resize(13, '\0');
  append(arc, name);
  const auto le = [&](std::uint32_t v, int n) {
    for (int i = 0; i < n; ++i) arc.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
  };
  le(static_cast<std::uint32_t>(rle.size()), 4);
  le(0x52CF, 2);  // 2021-06-15
  le(0x6000, 2);  // 12:00
  le(archive::crc16_arc(*plain), 2);
  le(static_cast<std::uint32_t>(plain->size()), 4);
  append(arc, rle);
  arc.push_back(std::byte{0x1A});
  arc.push_back(std::byte{0});
  const auto packed = test::read_as<ir::Archive>("arc", arc);
  REQUIRE_OK(packed);
  REQUIRE_EQ(packed->entries.size(), 1u);
  CHECK(packed->entries[0].path == "RUNS.TXT");
  CHECK(packed->entries[0].bytes == *plain);
  CHECK_EQ(packed->entries[0].mtime, tree::mtime);
  // Same entry with a wrong CRC.
  Bytes bad = arc;
  bad[23] ^= std::byte{1};
  CHECK_STATUS(test::read_as<ir::Archive>("arc", bad), Status::Decode);
  // Squeeze: a two-leaf tree ('x' = bit 0, end = bit 1), data 0b10 -> "x".
  const Bytes squeezed = {std::byte{1}, std::byte{0}, std::byte{0x87}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFE},
                          std::byte{0x02}};
  const auto x = lg::arc_unsqueeze(squeezed, 10);
  REQUIRE_OK(x);
  CHECK(test::str(*x) == "x");
  // A tree that loops forever is refused.
  Bytes looping = {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  looping.resize(200, std::byte{0});
  CHECK_STATUS(lg::arc_unsqueeze(looping, 10), Status::Decode);
  // Early crunching: garbage never crashes.
  for (int method = 5; method <= 7; ++method) {
    for (std::uint32_t seed = 1; seed < 20; ++seed) (void)lg::arc_uncrunch_old(test::random_bytes(300, seed), method, 100000);
  }
}

TEST_CASE("archive/legacy: damaged archives fail cleanly") {
  for (const char* name : {"tree_l0_lh0.lzh", "tree_l1_lh0.lzh", "tree_l2_lh0.lzh", "tree_l2_lh5.lzh"}) {
    test::fuzz_reader<ir::Archive>("lzh", legacy_fixture(name));
  }
  test::fuzz_reader<ir::Archive>("arj", legacy_fixture("tree_m0.arj"));
  test::fuzz_reader<ir::Archive>("arj", legacy_fixture("tree_m4.arj"));
  for (const char* name : {"flat.arc", "flat_squashed.arc"}) test::fuzz_reader<ir::Archive>("arc", legacy_fixture(name));
  // Bit flips in stored data are caught by the CRCs.
  Bytes lzh = legacy_fixture("tree_l2_lh0.lzh");
  const std::size_t hello = test::str(lzh).find("Hello, archive!");
  REQUIRE(hello != std::string::npos);
  lzh[hello] ^= std::byte{0x20};
  CHECK_STATUS(test::read_as<ir::Archive>("lzh", lzh), Status::Decode);
  Bytes arj = legacy_fixture("tree_m0.arj");
  const std::size_t text = test::str(arj).find("Lorem ipsum");
  REQUIRE(text != std::string::npos);
  arj[text] ^= std::byte{0x20};
  CHECK_STATUS(test::read_as<ir::Archive>("arj", arj), Status::Decode);
  // Garbled (encrypted) ARJ entries are refused with a precise message: set the flag
  // of the first file header and fix its header CRC.
  Bytes garbled = legacy_fixture("tree_m0.arj");
  const std::size_t main_size = static_cast<std::size_t>(std::to_integer<unsigned>(garbled[2]) | (std::to_integer<unsigned>(garbled[3]) << 8));
  const std::size_t file = 4 + main_size + 4 + 2;
  REQUIRE(garbled[file] == std::byte{0x60});
  const std::size_t basic = static_cast<std::size_t>(std::to_integer<unsigned>(garbled[file + 2]) | (std::to_integer<unsigned>(garbled[file + 3]) << 8));
  garbled[file + 4 + 4] |= std::byte{0x01};
  const std::uint32_t crc = util::crc32(ByteView(garbled).subspan(file + 4, basic));
  for (int i = 0; i < 4; ++i) garbled[file + 4 + basic + static_cast<std::size_t>(i)] = static_cast<std::byte>((crc >> (8 * i)) & 0xFFu);
  const auto refused = test::read_as<ir::Archive>("arj", garbled);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().message.find("encrypted") != std::string::npos);
  // Limits.
  Limits few;
  few.max_archive_entries = 2;
  CHECK_STATUS(test::read_as<ir::Archive>("lzh", legacy_fixture("tree_l2_lh0.lzh"), "", nullptr, few), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("arj", legacy_fixture("tree_m0.arj"), "", nullptr, few), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("arc", legacy_fixture("flat.arc"), "", nullptr, few), Status::Limit);
  Limits small;
  small.max_decompressed_bytes = 4000;
  CHECK_STATUS(test::read_as<ir::Archive>("arc", legacy_fixture("flat.arc"), "", nullptr, small), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("arj", legacy_fixture("tree_m0.arj"), "", nullptr, small), Status::Limit);
  // Garbage.
  for (const char* id : {"lzh", "arj", "arc"}) CHECK_FALSE(test::read_as<ir::Archive>(id, test::random_bytes(512, 4)).has_value());
}

TEST_CASE("archive/legacy: detection and conversion to tar") {
  CHECK(detect_id(legacy_fixture("tree_l2_lh0.lzh"), "") == "lzh");
  CHECK(detect_id(legacy_fixture("tree_l0_lh0.lzh"), "old.lha") == "lzh");
  CHECK(detect_id(legacy_fixture("tree_m0.arj"), "") == "arj");
  CHECK(detect_id(legacy_fixture("flat.arc"), "") == "arc");
  CHECK(detect_id(legacy_fixture("flat.arc"), "FILES.ARC") == "arc");
  for (const auto& [fixture, format] : {std::pair{"tree_l1_lh0.lzh", "lzh"}, std::pair{"tree_m0.arj", "arj"}, std::pair{"flat.arc", "arc"}}) {
    INFO("fixture ", fixture);
    const test::Converted out = test::convert(legacy_fixture(fixture), "", "tar");
    REQUIRE(out.ok());
    const auto back = test::read_as<ir::Archive>("tar", out.data);
    REQUIRE_OK(back);
    const auto original = test::read_as<ir::Archive>(format, legacy_fixture(fixture));
    REQUIRE_OK(original);
    std::size_t files = 0;
    for (const auto& e : original->entries) files += e.is_file() ? 1 : 0;
    std::size_t back_files = 0;
    for (const auto& e : back->entries) back_files += e.is_file() ? 1 : 0;
    CHECK_EQ(back_files, files);
  }
  const test::Converted zip = test::convert(legacy_fixture("tree_m0.arj"), "arj", "zip");
  CHECK(zip.ok());
}
