// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Microsoft Cabinet (gcab fixtures and hand-built cabinets) and Compiled HTML Help (chmcmd fixture).
#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/compression.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {

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

std::string detected(ByteView data) {
  const auto d = detect_format(Registry::instance(), data, "");
  return d ? d->format->id : std::string();
}

// ---- a small cabinet builder (stored folders) for the cases no tool on hand produces

void le16(Bytes& b, std::uint32_t v) {
  b.push_back(static_cast<std::byte>(v & 0xFF));
  b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}
void le32(Bytes& b, std::uint32_t v) {
  le16(b, v & 0xFFFF);
  le16(b, v >> 16);
}
void set32(Bytes& b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}

std::uint32_t cab_checksum(ByteView data, std::uint32_t seed) {
  std::uint32_t sum = seed;
  std::size_t i = 0;
  for (; i + 4 <= data.size(); i += 4) {
    sum ^= std::to_integer<std::uint32_t>(data[i]) | (std::to_integer<std::uint32_t>(data[i + 1]) << 8) |
           (std::to_integer<std::uint32_t>(data[i + 2]) << 16) | (std::to_integer<std::uint32_t>(data[i + 3]) << 24);
  }
  std::uint32_t tail = 0;
  for (; i < data.size(); ++i) tail = (tail << 8) | std::to_integer<std::uint32_t>(data[i]);
  return sum ^ tail;
}

struct CabFile {
  std::string name;
  std::string content;
  std::uint16_t folder = 0;
  std::uint16_t attributes = 0x20;
};

struct CabSpec {
  std::vector<std::vector<std::string>> folders;  // CFDATA payloads per folder
  std::vector<CabFile> files;                      // offsets are computed per folder
  bool reserve = false;
  std::string previous;
  std::string next;
  bool last_block_continues = false;  // the final block of the last folder goes on in the next cabinet
};

Bytes build_cab(const CabSpec& spec) {
  Bytes out;
  append(out, "MSCF");
  le32(out, 0);
  le32(out, 0);  // size, patched
  le32(out, 0);
  le32(out, 0);  // files offset, patched
  le32(out, 0);
  out.push_back(std::byte{3});
  out.push_back(std::byte{1});
  le16(out, static_cast<std::uint32_t>(spec.folders.size()));
  le16(out, static_cast<std::uint32_t>(spec.files.size()));
  std::uint16_t flags = 0;
  if (!spec.previous.empty()) flags |= 1;
  if (!spec.next.empty()) flags |= 2;
  if (spec.reserve) flags |= 4;
  le16(out, flags);
  le16(out, 0x1234);
  le16(out, spec.previous.empty() ? 0 : 1);
  const std::size_t data_reserve = spec.reserve ? 3 : 0;
  if (spec.reserve) {
    le16(out, 5);
    out.push_back(std::byte{2});
    out.push_back(std::byte{static_cast<unsigned char>(data_reserve)});
    out.insert(out.end(), 5, std::byte{0xAA});
  }
  const auto cstr = [&](const std::string& s) {
    append(out, s);
    out.push_back(std::byte{0});
  };
  if (!spec.previous.empty()) {
    cstr(spec.previous);
    cstr("disk");
  }
  if (!spec.next.empty()) {
    cstr(spec.next);
    cstr("disk");
  }
  std::vector<std::size_t> folder_at;
  for (const auto& f : spec.folders) {
    folder_at.push_back(out.size());
    le32(out, 0);
    le16(out, static_cast<std::uint32_t>(f.size()));
    le16(out, 0);  // stored
    if (spec.reserve) out.insert(out.end(), 2, std::byte{0xBB});
  }
  set32(out, 16, static_cast<std::uint32_t>(out.size()));
  std::vector<std::uint32_t> folder_offset(spec.folders.size(), 0);
  for (const CabFile& f : spec.files) {
    std::uint32_t offset = 0;
    const std::size_t index = f.folder >= 0xFFFD ? (f.folder == 0xFFFE ? spec.folders.size() - 1 : 0) : f.folder;
    if (f.folder != 0xFFFD && f.folder != 0xFFFF) {
      offset = folder_offset[index];
      folder_offset[index] += static_cast<std::uint32_t>(f.content.size());
    }
    le32(out, static_cast<std::uint32_t>(f.content.size()));
    le32(out, offset);
    le16(out, f.folder);
    le16(out, 0x52CF);  // 2021-06-15
    le16(out, 0x6000);  // 12:00:00
    le16(out, f.attributes);
    cstr(f.name);
  }
  for (std::size_t i = 0; i < spec.folders.size(); ++i) {
    set32(out, folder_at[i], static_cast<std::uint32_t>(out.size()));
    for (std::size_t k = 0; k < spec.folders[i].size(); ++k) {
      const std::string& payload = spec.folders[i][k];
      const bool continues = spec.last_block_continues && i + 1 == spec.folders.size() && k + 1 == spec.folders[i].size();
      Bytes head;
      le32(head, 0);
      le16(head, static_cast<std::uint32_t>(payload.size()));
      le16(head, continues ? 0 : static_cast<std::uint32_t>(payload.size()));
      head.insert(head.end(), data_reserve, std::byte{0xCC});
      set32(head, 0, cab_checksum(ByteView(head).subspan(4), cab_checksum(as_bytes(payload), 0)));
      append(out, head);
      append(out, payload);
    }
  }
  set32(out, 8, static_cast<std::uint32_t>(out.size()));
  return out;
}

}  // namespace

TEST_CASE("archive/cab: gcab cabinets extract exactly") {
  const auto plain = test::read_as<ir::Archive>("cab", test::fixture("archive/cab/plain.cab"));
  REQUIRE_OK(plain);
  REQUIRE_EQ(plain->entries.size(), 2u);
  CHECK(plain->entries[0].path == "hello.txt");
  CHECK(test::str(plain->entries[0].bytes) == "Hello, archive!\n");
  CHECK_EQ(plain->entries[0].mtime, tree::mtime);
  CHECK(plain->entries[1].path == "lorem.txt");
  CHECK(test::str(plain->entries[1].bytes) == lorem());
  CHECK(detected(test::fixture("archive/cab/plain.cab")) == "cab");

  if (!compression::can_decode(compression::Method::Mszip)) {
    MESSAGE("skipping mszip.cab: MSZIP codec not available yet");
    CHECK_STATUS(test::read_as<ir::Archive>("cab", test::fixture("archive/cab/mszip.cab")), Status::Decode);
    return;
  }
  const auto mszip = test::read_as<ir::Archive>("cab", test::fixture("archive/cab/mszip.cab"));
  REQUIRE_OK(mszip);
  REQUIRE_EQ(mszip->entries.size(), 3u);
  CHECK(test::str(find(*mszip, "hello.txt")->bytes) == "Hello, archive!\n");
  CHECK(test::str(find(*mszip, "lorem.txt")->bytes) == lorem());
  CHECK(find(*mszip, "noise.bin")->bytes == tree::noise(4096));
  for (const auto& e : mszip->entries) CHECK_EQ(e.mtime, tree::mtime);
}

TEST_CASE("archive/cab: several folders, reserved areas, names and attributes") {
  CabSpec spec;
  spec.reserve = true;
  spec.folders = {{"Hello, ", "archive!\n"}, {"second folder", " text"}};
  spec.files = {{"hello.txt", "Hello, archive!\n", 0, 0x20},
                {"dir\\caf\xC3\xA9.txt", "second folder", 1, 0xA0},   // UTF-8 flag
                {"na\xEFve.txt", " text", 1, 0x01 | 0x40}};           // Latin-1, read-only, executable
  const Bytes cab = build_cab(spec);
  const auto a = test::read_as<ir::Archive>("cab", cab);
  REQUIRE_OK(a);
  REQUIRE_EQ(a->entries.size(), 3u);
  CHECK(test::str(find(*a, "hello.txt")->bytes) == "Hello, archive!\n");
  REQUIRE(find(*a, "dir/caf\xC3\xA9.txt") != nullptr);
  CHECK(test::str(find(*a, "dir/caf\xC3\xA9.txt")->bytes) == "second folder");
  const auto* naive = find(*a, "na\xC3\xAFve.txt");
  REQUIRE(naive != nullptr);
  CHECK(test::str(naive->bytes) == " text");
  CHECK_EQ(naive->unix_mode, 0555u);
  CHECK_EQ(find(*a, "hello.txt")->unix_mode, 0644u);
  // A damaged data block fails its checksum.
  Bytes bad = cab;
  bad[bad.size() - 3] ^= std::byte{0x01};
  CHECK_STATUS(test::read_as<ir::Archive>("cab", bad), Status::Decode);
  test::fuzz_reader<ir::Archive>("cab", cab);
}

TEST_CASE("archive/cab: files spanning cabinets are reported and skipped") {
  CabSpec spec;
  spec.previous = "part1.cab";
  spec.next = "part3.cab";
  spec.folders = {{"tail of a", "whole"}, {"inner", "start of b"}};
  spec.last_block_continues = true;
  spec.files = {{"a.bin", "tail of a", 0xFFFD, 0x20},
                {"whole.txt", "whole", 0, 0x20},
                {"inner.txt", "inner", 1, 0x20},
                {"b.bin", "start of b and more", 0xFFFE, 0x20}};
  std::vector<std::string> warnings;
  const auto a = test::read_as<ir::Archive>("cab", build_cab(spec), "", &warnings);
  REQUIRE_OK(a);
  // The first folder starts in part1.cab, so nothing in it is complete; the last goes on in part3.cab.
  REQUIRE_EQ(a->entries.size(), 1u);
  CHECK(a->entries[0].path == "inner.txt");
  CHECK(test::str(a->entries[0].bytes) == "inner");
  std::string all;
  for (const auto& w : warnings) all += w + "\n";
  CHECK(all.find("previous cabinet 'part1.cab'") != std::string::npos);
  CHECK(all.find("'a.bin'") != std::string::npos);
  CHECK(all.find("'whole.txt'") != std::string::npos);
  CHECK(all.find("next cabinet 'part3.cab'") != std::string::npos);
  CHECK(all.find("'b.bin'") != std::string::npos);
}

TEST_CASE("archive/cab: limits, garbage and conversion") {
  test::fuzz_reader<ir::Archive>("cab", test::fixture("archive/cab/plain.cab"));
  test::fuzz_reader<ir::Archive>("cab", test::fixture("archive/cab/mszip.cab"));
  CHECK_STATUS(test::read_as<ir::Archive>("cab", test::random_bytes(500, 4)), Status::Decode);
  Limits few;
  few.max_archive_entries = 1;
  CHECK_STATUS(test::read_as<ir::Archive>("cab", test::fixture("archive/cab/plain.cab"), "", nullptr, few), Status::Limit);
  Limits small;
  small.max_decompressed_bytes = 100;
  CHECK_STATUS(test::read_as<ir::Archive>("cab", test::fixture("archive/cab/plain.cab"), "", nullptr, small), Status::Limit);
  const test::Converted zip = test::convert(test::fixture("archive/cab/plain.cab"), "", "zip");
  REQUIRE(zip.ok());
  const auto back = test::read_as<ir::Archive>("zip", zip.data);
  REQUIRE_OK(back);
  CHECK(test::str(find(*back, "lorem.txt")->bytes) == lorem());
  // Read only: nothing converts to cab or chm.
  CHECK(test::find_writer("cab", Kind::Archive) == nullptr);
  CHECK(test::find_writer("chm", Kind::Archive) == nullptr);
}

TEST_CASE("archive/chm: chmcmd help file lists its pages") {
  const Bytes chm = test::fixture("archive/cab/demo.chm");
  CHECK(detected(chm) == "chm");
  test::fuzz_reader<ir::Archive>("chm", chm);
  CHECK_STATUS(test::read_as<ir::Archive>("chm", test::random_bytes(300, 2)), Status::Decode);
  if (!compression::can_decode(compression::Method::Lzx)) {
    MESSAGE("skipping demo.chm content: LZX codec not available yet");
    CHECK_STATUS(test::read_as<ir::Archive>("chm", chm), Status::Decode);
    return;
  }
  const auto a = test::read_as<ir::Archive>("chm", chm);
  REQUIRE_OK(a);
  const auto* index = find(*a, "index.html");
  REQUIRE(index != nullptr);
  CHECK(test::str(index->bytes) == "<html><body><h1>Index</h1><p>Hello, archive!</p></body></html>\n");
  const auto* page = find(*a, "sub/page.html");
  REQUIRE(page != nullptr);
  CHECK(test::str(page->bytes) == "<html><body><p>Sub page</p></body></html>\n");
  const auto* text = find(*a, "lorem.html");
  REQUIRE(text != nullptr);
  std::string expected = "<html><body><pre>";
  for (int i = 0; i < 40; ++i) expected += tree::lorem_paragraph();
  expected += "</pre></body></html>\n";
  CHECK(test::str(text->bytes) == expected);
  for (const auto& e : a->entries) {
    CHECK_FALSE(e.path.starts_with("#"));
    CHECK_FALSE(e.path.starts_with("$"));
    CHECK_FALSE(e.path.starts_with(":"));
  }
  Limits small;
  small.max_decompressed_bytes = 1000;
  CHECK_STATUS(test::read_as<ir::Archive>("chm", chm, "", nullptr, small), Status::Limit);
  const test::Converted zip = test::convert(chm, "", "zip");
  CHECK(zip.ok());
}
