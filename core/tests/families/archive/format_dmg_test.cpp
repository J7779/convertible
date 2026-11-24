// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Apple disk images (UDIF) with HFS+ / HFSX volumes, made by hdiutil.
#include <algorithm>

#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/hfs_plus.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {
Bytes dmg(std::string_view name) {
  Bytes data = test::fixture(std::string("archive/dmg/") + std::string(name));
  if (name.ends_with(".gz")) {
    auto plain = compression::decompress(compression::Method::Gzip, data);
    REQUIRE_OK(plain);
    data = std::move(*plain);
  }
  return data;
}

ir::Archive read_ok(ByteView data, std::string_view name) {
  INFO("fixture ", name);
  auto archive = test::read_as<ir::Archive>("dmg", data);
  REQUIRE_OK(archive);
  return std::move(*archive);
}

const ir::ArchiveEntry* find(const ir::Archive& a, std::string_view path) {
  for (const auto& e : a.entries) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

// Patch a big-endian u32 of the koly trailer.
void set_koly32(Bytes& image, std::size_t field, std::uint32_t value) {
  const std::size_t at = image.size() - 512 + field;
  for (int i = 0; i < 4; ++i) image[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((value >> (24 - 8 * i)) & 0xFFu);
}
}  // namespace

TEST_CASE("archive/dmg: hdiutil images extract exactly with every chunk compression") {
  struct Case {
    const char* fixture;
    compression::Method method;
  };
  for (const Case& c : {Case{"tree_UDZO.dmg", compression::Method::Zlib}, Case{"tree_UDBZ.dmg", compression::Method::Bzip2},
                        Case{"tree_ULMO.dmg", compression::Method::Xz}, Case{"tree_ULFO.dmg", compression::Method::Lzfse},
                        Case{"tree_UDCO.dmg.gz", compression::Method::Adc}, Case{"tree_UDRO.dmg.gz", compression::Method::Store},
                        Case{"tree_apm_ULMO.dmg", compression::Method::Xz}}) {
    INFO("fixture ", c.fixture);
    const Bytes image = dmg(c.fixture);
    if (!compression::can_decode(c.method)) {
      MESSAGE("skipping ", c.fixture, ": ", compression::method_name(c.method), " not available yet");
      const auto r = test::read_as<ir::Archive>("dmg", image);
      REQUIRE_FALSE(r.has_value());
      CHECK(r.error().message.find("not available") != std::string::npos);
      continue;
    }
    const ir::Archive archive = read_ok(image, c.fixture);
    // Everything, including the decomposed (NFD) unicode name read back precomposed.
    tree::check(archive);
    for (const auto& e : archive.entries) {
      CHECK(e.path.find("HFS+ Private") == std::string::npos);
      if (e.is_directory) {
        CHECK_EQ(e.unix_mode, 0755u);
        CHECK_EQ(e.mtime, tree::mtime);
      }
    }
  }
}

TEST_CASE("archive/dmg: HFSX without a partition map, hard links and HFS compression") {
  const ir::Archive a = read_ok(dmg("links_hfsx_UDZO.dmg"), "links_hfsx_UDZO.dmg");
  const auto* first = find(a, "a.txt");
  const auto* second = find(a, "b.txt");
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(test::str(first->bytes) == "shared content\n");
  CHECK(test::str(second->bytes) == "shared content\n");
  CHECK_EQ(first->unix_mode, 0644u);
  std::string lorem;
  for (int i = 0; i < 40; ++i) lorem += tree::lorem_paragraph();
  const auto* plain = find(a, "plain.txt");
  REQUIRE(plain != nullptr);
  CHECK(test::str(plain->bytes) == lorem);
  CHECK_EQ(plain->mtime, tree::mtime);
  for (const auto& e : a.entries) CHECK(e.path.find("iNode") == std::string::npos);
  // packed.txt is stored with LZVN HFS compression (decoded through the LZFSE codec).
  const auto* packed = find(a, "packed.txt");
  if (compression::can_decode(compression::Method::Lzfse)) {
    REQUIRE(packed != nullptr);
    CHECK(test::str(packed->bytes) == lorem);
  } else {
    // Without the decoder the file is skipped with a note, the rest is still read.
    CHECK(packed == nullptr);
    std::vector<std::string> warnings;
    REQUIRE_OK(test::read_as<ir::Archive>("dmg", dmg("links_hfsx_UDZO.dmg"), "", &warnings));
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) { return w.find("LZVN") != std::string::npos; }));
  }
}

TEST_CASE("archive/dmg: damaged, hostile and unsupported images fail cleanly") {
  const Bytes image = dmg("tree_UDZO.dmg");
  test::fuzz_reader<ir::Archive>("dmg", image, 128);
  test::fuzz_reader<ir::Archive>("dmg", dmg("links_hfsx_UDZO.dmg"), 128);
  test::fuzz_reader<ir::Archive>("dmg", dmg("tree_apm_ULMO.dmg"), 128);
  CHECK_FALSE(test::read_as<ir::Archive>("dmg", test::random_bytes(4096, 8)).has_value());
  // Encrypted images.
  Bytes encrypted = test::bytes("encrcdsa");
  encrypted.resize(4096, std::byte{0});
  const auto enc = test::read_as<ir::Archive>("dmg", encrypted);
  REQUIRE_FALSE(enc.has_value());
  CHECK(enc.error().message.find("encrypted") != std::string::npos);
  // Segmented images.
  Bytes segmented = image;
  set_koly32(segmented, 60, 3);
  CHECK_STATUS(test::read_as<ir::Archive>("dmg", segmented), Status::Decode);
  // An APFS partition (the GPT type of the HFS+ partition changed to APFS) gives a precise error.
  Bytes raw = dmg("tree_UDRO.dmg.gz");
  const std::size_t entries = 1024;  // primary GPT entries at LBA 2
  bool patched = false;
  for (std::size_t i = 0; i < 128 && !patched; ++i) {
    const std::size_t at = entries + i * 128;
    if (raw[at] == std::byte{0x00} && raw[at + 1] == std::byte{0x53} && raw[at + 2] == std::byte{0x46} && raw[at + 3] == std::byte{0x48}) {
      const std::uint8_t apfs[4] = {0xEF, 0x57, 0x34, 0x7C};
      for (std::size_t k = 0; k < 4; ++k) raw[at + k] = static_cast<std::byte>(apfs[k]);
      patched = true;
    }
  }
  REQUIRE(patched);
  // The raw image is UDRO (raw chunks): the patched bytes are what the reader sees.
  const auto apfs = test::read_as<ir::Archive>("dmg", raw);
  REQUIRE_FALSE(apfs.has_value());
  CHECK(apfs.error().message.find("APFS") != std::string::npos);
  // Limits: entries and decompressed bytes.
  Limits few;
  few.max_archive_entries = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("dmg", image, "", nullptr, few), Status::Limit);
  Limits small;
  small.max_decompressed_bytes = 4000;
  CHECK_STATUS(test::read_as<ir::Archive>("dmg", image, "", nullptr, small), Status::Limit);
}

TEST_CASE("archive/dmg: HFS+ names are recomposed to NFC") {
  CHECK(archive::hfs::name_to_utf8(u"über") == "\xC3\xBC" "ber");
  CHECK(archive::hfs::name_to_utf8(u"각") == "\xEA\xB0\x81");  // Hangul: GAG
  CHECK(archive::hfs::name_to_utf8(u"が") == "\xE3\x81\x8C");          // ka + dakuten -> ga
  CHECK(archive::hfs::name_to_utf8(u"a/b") == "a:b");
}

TEST_CASE("archive/dmg: detection and conversion to tar") {
  const Bytes image = dmg("tree_UDZO.dmg");
  const auto detected = detect_format(Registry::instance(), image, "");
  REQUIRE(detected.has_value());
  CHECK(detected->format->id == "dmg");
  CHECK(detect_format(Registry::instance(), image, "App.dmg")->format->id == "dmg");
  const test::Converted out = test::convert(image, "", "tar");
  REQUIRE(out.ok());
  const auto back = test::read_as<ir::Archive>("tar", out.data);
  REQUIRE_OK(back);
  tree::check(*back);
  // A raw disk image (no UDIF trailer, as written by hdiutil -format UDRW) is read too.
  const Bytes raw = dmg("links_hfsx_raw.dmg.gz");
  CHECK(detect_format(Registry::instance(), raw, "links.dmg")->format->id == "dmg");
  const ir::Archive links = read_ok(raw, "links_hfsx_raw.dmg.gz");
  const auto* b = find(links, "b.txt");
  REQUIRE(b != nullptr);
  CHECK(test::str(b->bytes) == "shared content\n");
}
