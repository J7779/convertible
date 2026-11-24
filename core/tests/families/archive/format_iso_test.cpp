// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// ISO 9660 (Rock Ridge, Joliet, zisofs) and UDF images: reading real images, writing, interop.
#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "families/archive/compression.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {

Bytes gunzip(std::string_view fixture) {
  auto out = compression::decompress(compression::Method::Gzip, test::fixture(std::string("archive/") + std::string(fixture)));
  REQUIRE_OK(out);
  return std::move(*out);
}

ir::Archive read_iso(ByteView image, std::string_view options = {}) {
  auto archive = test::read_as<ir::Archive>("iso", image, options);
  REQUIRE_OK(archive);
  return std::move(*archive);
}

const ir::ArchiveEntry* find(const ir::Archive& a, std::string_view path) {
  for (const auto& e : a.entries) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

std::string lorem() {
  std::string text;
  for (int i = 0; i < 8; ++i) text += tree::lorem_paragraph();
  return text;
}

}  // namespace

TEST_CASE("archive/iso: xorriso image with Rock Ridge and Joliet extracts exactly") {
  const Bytes image = gunzip("iso/tree_rr_joliet.iso.gz");
  // Rock Ridge: names, permissions, times and the symbolic link.
  tree::check(read_iso(image));
  CHECK(test::str(image).find("CD001") != std::string::npos);
  // The explicit primary tree is the Rock Ridge one as well.
  tree::check(read_iso(image, R"({"iso_filesystem":"iso9660"})"));
  // Joliet: names (the 124-byte one is cut to 103 characters) and times, no links or permissions.
  const ir::Archive joliet = read_iso(image, R"({"iso_filesystem":"joliet"})");
  tree::check(joliet, {.symlink = false, .modes = false, .long_name = false, .allow_extra = true});
  bool long_found = false;
  for (const auto& e : joliet.entries) {
    if (e.path.starts_with("long/long_file_name_")) {
      long_found = true;
      CHECK(test::str(e.bytes) == "long name\n");
    }
  }
  CHECK(long_found);
  // No UDF on this image.
  CHECK_STATUS(test::read_as<ir::Archive>("iso", image, R"({"iso_filesystem":"udf"})"), Status::Decode);
}

TEST_CASE("archive/iso: plain level 1 image without extensions") {
  const ir::Archive plain = read_iso(gunzip("iso/plain.iso.gz"));
  REQUIRE_EQ(plain.entries.size(), 2u);
  const auto* hello = find(plain, "hello.txt");
  REQUIRE(hello != nullptr);
  CHECK(test::str(hello->bytes) == "Hello, archive!\n");
  CHECK_EQ(hello->mtime, tree::mtime);
  const auto* text = find(plain, "lorem.txt");
  REQUIRE(text != nullptr);
  CHECK(test::str(text->bytes) == lorem());
  CHECK_STATUS(test::read_as<ir::Archive>("iso", gunzip("iso/plain.iso.gz"), R"({"iso_filesystem":"joliet"})"), Status::Decode);
}

TEST_CASE("archive/iso: zisofs compressed files are expanded") {
  const ir::Archive z = read_iso(gunzip("iso/zisofs.iso.gz"));
  const auto* text = find(z, "lorem.txt");
  REQUIRE(text != nullptr);
  CHECK(test::str(text->bytes) == lorem());
  CHECK_EQ(text->mtime, tree::mtime);
  CHECK_EQ(text->unix_mode, 0644u);
  const auto* hello = find(z, "hello.txt");
  REQUIRE(hello != nullptr);
  CHECK(test::str(hello->bytes) == "Hello, archive!\n");
}

TEST_CASE("archive/udf: genisoimage UDF tree is read in preference to bare ISO 9660 names") {
  const Bytes image = gunzip("iso/udf_hybrid.iso.gz");
  const tree::Check udf_subset{.symlink = false, .modes = false, .long_name = false};
  tree::check(read_iso(image), udf_subset);
  tree::check(read_iso(image, R"({"iso_filesystem":"udf"})"), udf_subset);
  // The ISO 9660 side of the same disc has only 8.3 names.
  const ir::Archive iso9660 = read_iso(image, R"({"iso_filesystem":"iso9660"})");
  CHECK(find(iso9660, "hello.txt") != nullptr);
  // A UDF-only disc: drop the ISO 9660 descriptors from the recognition sequence.
  Bytes udf_only = image;
  for (std::size_t s = 16; s < 32; ++s) {
    const std::size_t at = s * 2048;
    if (test::str(ByteView(udf_only).subspan(at + 1, 5)) == "CD001") {
      const char* bea = "\0BEA01\1";
      for (std::size_t i = 0; i < 7; ++i) udf_only[at + i] = static_cast<std::byte>(bea[i]);
      std::fill(udf_only.begin() + static_cast<std::ptrdiff_t>(at + 7), udf_only.begin() + static_cast<std::ptrdiff_t>(at + 2048), std::byte{0});
    }
  }
  tree::check(read_iso(udf_only), udf_subset);
  const test::Converted detected = test::convert(udf_only, "", "zip");
  CHECK(detected.ok());
  test::fuzz_reader<ir::Archive>("iso", udf_only, 256);
}

TEST_CASE("archive/iso: writer round trips with Rock Ridge, Joliet and deterministic output") {
  const ir::Archive source = tree::archive();
  const auto written = test::write_from("iso", source);
  REQUIRE_OK(written);
  CHECK_EQ(written->size() % 2048, 0u);
  tree::check(read_iso(*written));
  // Joliet tree of our output: names up to 64 characters, no links.
  const ir::Archive joliet = read_iso(*written, R"({"iso_filesystem":"joliet"})");
  tree::check(joliet, {.symlink = false, .modes = false, .long_name = false, .allow_extra = true});
  // Deterministic, and no owner names or host data.
  CHECK(*test::write_from("iso", source) == *written);
  // Volume label (default CDROM).
  CHECK(test::str(ByteView(*written).subspan(32808, 32)).starts_with("CDROM "));
  const auto labelled = test::write_from("iso", source, R"({"volume_label":"My disc"})");
  REQUIRE_OK(labelled);
  CHECK(test::str(ByteView(*labelled).subspan(32808, 32)).starts_with("MY_DISC "));
  // The volume dates are the newest modification time, not the clock.
  CHECK(test::str(ByteView(*written).subspan(32768 + 813, 16)) == "2021061512000000");
  const auto fixed = test::write_from("iso", source, R"({"timestamps":"fixed"})");
  REQUIRE_OK(fixed);
  for (const auto& e : read_iso(*fixed).entries) CHECK_EQ(e.mtime, 315532800);
}

TEST_CASE("archive/iso: writer edge cases") {
  // Empty archive.
  const auto empty = test::write_from("iso", ir::Archive{});
  REQUIRE_OK(empty);
  CHECK(read_iso(*empty).entries.empty());
  // Clashing ISO names, very long names and link targets, a deep tree and a 1 MB file.
  ir::Archive a;
  const auto add = [&](std::string path, Bytes bytes) {
    ir::ArchiveEntry e;
    e.path = std::move(path);
    e.bytes = std::move(bytes);
    e.mtime = tree::mtime;
    e.unix_mode = 0644;
    a.entries.push_back(std::move(e));
  };
  add("a-b.txt", test::bytes("1"));
  add("a_b.txt", test::bytes("2"));
  add("A B.TXT", test::bytes("3"));
  const std::string long_name(240, 'n');
  add(long_name, test::bytes("long"));
  std::string deep;
  for (int i = 0; i < 12; ++i) deep += "level" + std::to_string(i) + "/";
  add(deep + "file.txt", test::bytes("deep"));
  add("big.bin", test::random_bytes(1u << 20, 7));
  ir::ArchiveEntry link;
  link.path = "far";
  link.symlink_target = "level0/level1/level2/level3/level4/level5/level6/level7/level8/level9/level10/level11/" + std::string(200, 'x');
  a.entries.push_back(link);
  const auto written = test::write_from("iso", a);
  REQUIRE_OK(written);
  const ir::Archive back = read_iso(*written);
  CHECK(test::str(find(back, "a-b.txt")->bytes) == "1");
  CHECK(test::str(find(back, "a_b.txt")->bytes) == "2");
  CHECK(test::str(find(back, "A B.TXT")->bytes) == "3");
  REQUIRE(find(back, long_name) != nullptr);
  REQUIRE(find(back, deep + "file.txt") != nullptr);
  CHECK(find(back, "big.bin")->bytes == test::random_bytes(1u << 20, 7));
  REQUIRE(find(back, "far") != nullptr);
  CHECK(find(back, "far")->symlink_target == link.symlink_target);
  const ir::Archive joliet = read_iso(*written, R"({"iso_filesystem":"joliet"})");
  std::size_t files = 0;
  for (const auto& e : joliet.entries) files += e.is_file() ? 1 : 0;
  CHECK_EQ(files, 6u);
}

TEST_CASE("archive/iso: conversions, detection and damaged images") {
  // iso -> zip and tar -> iso through the engine.
  const test::Converted zip = test::convert(gunzip("iso/tree_rr_joliet.iso.gz"), "", "zip");
  REQUIRE(zip.ok());
  tree::check(*test::read_as<ir::Archive>("zip", zip.data));
  const test::Converted iso = test::convert(test::fixture("archive/tar/tree_pax.tar"), "tar", "iso");
  REQUIRE(iso.ok());
  CHECK(iso.format_id == "iso");
  tree::check(*test::read_as<ir::Archive>("iso", iso.data));
  // Same-format rewrite.
  const test::Converted again = test::convert(iso.data, "", "iso");
  REQUIRE(again.ok());
  tree::check(*test::read_as<ir::Archive>("iso", again.data));
  // Damaged input.
  test::fuzz_reader<ir::Archive>("iso", gunzip("iso/tree_rr_joliet.iso.gz"), 256);
  test::fuzz_reader<ir::Archive>("iso", gunzip("iso/zisofs.iso.gz"), 128);
  test::fuzz_reader<ir::Archive>("iso", *test::write_from("iso", tree::archive()), 128);
  CHECK_STATUS(test::read_as<ir::Archive>("iso", test::random_bytes(40000, 3)), Status::Decode);
  Limits few;
  few.max_archive_entries = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("iso", gunzip("iso/tree_rr_joliet.iso.gz"), "", nullptr, few), Status::Limit);
  Limits small;
  small.max_decompressed_bytes = 1000;
  CHECK_STATUS(test::read_as<ir::Archive>("iso", gunzip("iso/tree_rr_joliet.iso.gz"), "", nullptr, small), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("iso", gunzip("iso/zisofs.iso.gz"), "", nullptr, small), Status::Limit);
}
