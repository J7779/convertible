// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// RAR 5 and RAR 2.9-4.x reading: real archives from rar 7.01 and rar 6.23.
#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/formats/rar_internal.hpp"
#include "util/crc.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {
Bytes rar_fixture(std::string_view name) { return test::fixture(std::string("archive/rar/") + std::string(name)); }

ir::Archive read_ok(std::string_view fixture, std::string_view options = {}) {
  INFO("fixture ", fixture);
  auto archive = test::read_as<ir::Archive>("rar", rar_fixture(fixture), options);
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

// The synthetic filter inputs of SOURCES.md (tools/make_inputs.py): size and CRC-32.
struct Input {
  const char* name;
  std::size_t size;
  std::uint32_t crc;
};
constexpr Input x86_bin{"x86.bin", 24576, 0xE1041618u};
constexpr Input rgb_bmp{"rgb.bmp", 12342, 0x8F198288u};
constexpr Input pcm_wav{"pcm.wav", 24044, 0x3C982977u};
constexpr Input table_dat{"table.dat", 24000, 0x280E6CC3u};
constexpr Input text_txt{"text.txt", 20000, 0xAE8FA3E9u};
constexpr Input far_bin{"far.bin", 3832768, 0xF24AC78Bu};
constexpr Input wrap_bin{"wrap.bin", 1040416, 0x2FA6C1D8u};
constexpr Input sparse_exe{"sparse.exe", 655360, 0xED13E15Eu};

void check_input(const ir::Archive& a, const Input& in) {
  INFO("entry ", in.name);
  const ir::ArchiveEntry* e = find(a, in.name);
  REQUIRE(e != nullptr);
  CHECK_EQ(e->bytes.size(), in.size);
  CHECK_EQ(util::crc32(e->bytes), in.crc);
  CHECK_EQ(e->mtime, tree::mtime);
}

// ---- hand-built archives

void put_vint(Bytes& out, std::uint64_t v) {
  do {
    auto b = static_cast<std::uint8_t>(v & 0x7Fu);
    v >>= 7;
    if (v != 0) b |= 0x80u;
    out.push_back(static_cast<std::byte>(b));
  } while (v != 0);
}

/// A RAR 5 header: CRC-32, size, then `body` (type, flags, fields).
Bytes rar5_header(const Bytes& body) {
  Bytes sized;
  put_vint(sized, body.size());
  sized.insert(sized.end(), body.begin(), body.end());
  const std::uint32_t crc = util::crc32(sized);
  Bytes out;
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((crc >> (8 * i)) & 0xFFu));
  out.insert(out.end(), sized.begin(), sized.end());
  return out;
}

/// A RAR 5 archive with one compressed file whose header says what the test wants.
Bytes rar5_archive(std::uint64_t file_flags, std::uint64_t unpacked, std::uint64_t compression, const Bytes& data) {
  Bytes out = to_bytes(std::string_view("Rar!\x1A\x07\x01\x00", 8));
  const Bytes main = rar5_header({std::byte{1}, std::byte{0}, std::byte{0}});
  out.insert(out.end(), main.begin(), main.end());
  Bytes file;
  put_vint(file, 2);  // file header
  put_vint(file, 2);  // data area present
  put_vint(file, data.size());
  put_vint(file, file_flags);
  put_vint(file, unpacked);
  put_vint(file, 0);  // attributes
  put_vint(file, compression);
  put_vint(file, 1);  // Unix
  put_vint(file, 1);
  file.push_back(std::byte{'a'});
  const Bytes header = rar5_header(file);
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), data.begin(), data.end());
  const Bytes end = rar5_header({std::byte{5}, std::byte{0}, std::byte{0}});
  out.insert(out.end(), end.begin(), end.end());
  return out;
}

/// Rewrite one byte of the RAR 4 file header of `name` and fix the header CRC.
Bytes patch_rar4(Bytes archive, std::string_view name, std::size_t offset, std::uint8_t value) {
  std::size_t p = 7;
  const auto u16 = [&](std::size_t at) {
    return std::to_integer<std::size_t>(archive[at]) | (std::to_integer<std::size_t>(archive[at + 1]) << 8);
  };
  while (p + 7 <= archive.size()) {
    const std::size_t size = u16(p + 5);
    const auto type = std::to_integer<int>(archive[p + 2]);
    const std::size_t flags = u16(p + 3);
    std::size_t add = 0;
    if (type == 0x74 || (flags & 0x8000u) != 0) {
      add = u16(p + 7) | (u16(p + 9) << 16);
    }
    if (type == 0x74 && test::str(ByteView(archive).subspan(p + 32, u16(p + 26))) == name) {
      archive[p + offset] = static_cast<std::byte>(value);
      const std::uint32_t crc = util::crc32(ByteView(archive).subspan(p + 2, size - 2));
      archive[p] = static_cast<std::byte>(crc & 0xFFu);
      archive[p + 1] = static_cast<std::byte>((crc >> 8) & 0xFFu);
      return archive;
    }
    p += size + add;
  }
  FAIL("entry not found in the RAR 4 archive");
  return archive;
}
}  // namespace

// ============================================================================= RAR 5

TEST_CASE("archive/rar: RAR 5 archives at every method extract exactly") {
  for (const char* name : {"rar5_tree_m0.rar", "rar5_tree_m1.rar", "rar5_tree_m2.rar", "rar5_tree_m3.rar",
                           "rar5_tree_m4.rar", "rar5_tree_m5.rar", "rar5_tree_solid.rar"}) {
    INFO("fixture ", name);
    tree::check(read_ok(name));
  }
}

TEST_CASE("archive/rar: RAR 5 small dictionary, BLAKE2sp hashes, links and file copies") {
  const ir::Archive small = read_ok("rar5_dict128k.rar");
  REQUIRE_EQ(small.entries.size(), 3u);
  CHECK(test::str(find(small, "lorem.txt")->bytes) == lorem());
  CHECK(find(small, "noise.bin")->bytes == tree::noise(4096));
  CHECK(test::str(find(small, "hello.txt")->bytes) == "Hello, archive!\n");

  const ir::Archive hashed = read_ok("rar5_blake2.rar");
  CHECK(find(hashed, "noise.bin")->bytes == tree::noise(4096));
  CHECK(test::str(find(hashed, "lorem.txt")->bytes) == lorem());

  const ir::Archive links = read_ok("rar5_links.rar");
  REQUIRE_EQ(links.entries.size(), 4u);
  CHECK(test::str(find(links, "hard.txt")->bytes) == "Hello, archive!\n");
  CHECK(test::str(find(links, "copy.txt")->bytes) == lorem());
  CHECK_EQ(find(links, "copy.txt")->mtime, tree::mtime);
}

TEST_CASE("archive/rar: RAR 5 filters on executable, image, audio and table data") {
  const ir::Archive a = read_ok("rar5_filters.rar");
  REQUIRE_EQ(a.entries.size(), 4u);
  for (const Input& in : {x86_bin, rgb_bmp, pcm_wav, table_dat}) check_input(a, in);
  check_input(read_ok("rar5_text.rar"), text_txt);
  check_input(read_ok("rar5_sparse_x86.rar"), sparse_exe);  // filter blocks deep into the file
}

TEST_CASE("archive/rar: long distances and a dictionary that wraps around") {
  check_input(read_ok("rar5_far.rar"), far_bin);
  check_input(read_ok("rar4_far.rar"), far_bin);
  check_input(read_ok("rar4_wrap.rar"), wrap_bin);  // 64 KiB dictionary over 1 MB
}

TEST_CASE("archive/rar: RAR 5 comment and encryption") {
  CHECK(read_ok("rar5_comment.rar").comment == "Synthetic RAR test archive comment.\n");
  for (const char* name : {"rar5_pass.rar", "rar5_pass_headers.rar"}) {
    INFO("fixture ", name);
    const ir::Archive a = read_ok(name, R"({"password":"secret"})");
    REQUIRE_EQ(a.entries.size(), 2u);
    CHECK(test::str(find(a, "lorem.txt")->bytes) == lorem());
    CHECK(test::str(find(a, "hello.txt")->bytes) == "Hello, archive!\n");
    const auto wrong = test::read_as<ir::Archive>("rar", rar_fixture(name), R"({"password":"guess"})");
    CHECK_STATUS(wrong, Status::Decode);
    if (!wrong) CHECK_MESSAGE(wrong.error().message.find("wrong password") != std::string::npos, wrong.error().message);
    const auto none = test::read_as<ir::Archive>("rar", rar_fixture(name));
    CHECK_STATUS(none, Status::Decode);
    if (!none) CHECK(none.error().message.find("password required") != std::string::npos);
  }
}

// ============================================================================= RAR 4

TEST_CASE("archive/rar: RAR 4 archives at every method extract exactly") {
  for (const char* name : {"rar4_tree_m1.rar", "rar4_tree_m2.rar", "rar4_tree_m3.rar", "rar4_tree_m4.rar",
                           "rar4_tree_m5.rar", "rar4_tree_solid.rar"}) {
    INFO("fixture ", name);
    tree::check(read_ok(name));
  }
}

TEST_CASE("archive/rar: RAR 4 standard VM filters") {
  check_input(read_ok("rar4_filter_e8.rar"), x86_bin);
  check_input(read_ok("rar4_sparse_x86.rar"), sparse_exe);  // filter blocks deep into the file
  check_input(read_ok("rar4_text.rar"), text_txt);
  check_input(read_ok("rar4_filter_delta.rar"), table_dat);
  check_input(read_ok("rar4_filter_rgb.rar"), rgb_bmp);
  check_input(read_ok("rar4_filter_audio.rar"), pcm_wav);
}

TEST_CASE("archive/rar: RAR 4 PPMd blocks") {
  auto a = test::read_as<ir::Archive>("rar", rar_fixture("rar4_ppmd.rar"));
  if (!a) {
    CHECK(a.error().message.find("PPMd blocks are not available in this build") != std::string::npos);
    MESSAGE("PPMd model not available yet: ", a.error().message);
    return;
  }
  check_input(*a, text_txt);
}

TEST_CASE("archive/rar: RAR 4 comment and encryption") {
  CHECK(read_ok("rar4_comment.rar").comment == "Synthetic RAR test archive comment.\n");
  // 40 characters: RAR's key derivation rewrites the hashed buffer for passwords this long.
  const ir::Archive long_password = read_ok("rar4_pass_long.rar", R"({"password":"0123456789abcdefghijklmnopqrstuvwxyzABCD"})");
  CHECK(test::str(find(long_password, "lorem.txt")->bytes) == lorem());
  for (const char* name : {"rar4_pass.rar", "rar4_pass_headers.rar"}) {
    INFO("fixture ", name);
    const ir::Archive a = read_ok(name, R"({"password":"secret"})");
    REQUIRE_EQ(a.entries.size(), 2u);
    CHECK(test::str(find(a, "lorem.txt")->bytes) == lorem());
    CHECK(test::str(find(a, "hello.txt")->bytes) == "Hello, archive!\n");
    const auto wrong = test::read_as<ir::Archive>("rar", rar_fixture(name), R"({"password":"guess"})");
    CHECK_STATUS(wrong, Status::Decode);
    if (!wrong) CHECK_MESSAGE(wrong.error().message.find("wrong password") != std::string::npos, wrong.error().message);
    const auto none = test::read_as<ir::Archive>("rar", rar_fixture(name));
    CHECK_STATUS(none, Status::Decode);
    if (!none) CHECK(none.error().message.find("password required") != std::string::npos);
  }
}

// ============================================================================= both

TEST_CASE("archive/rar: multi-volume archives fail precisely") {
  for (const char* name : {"rar5_volume.part1.rar", "rar4_volume.part1.rar"}) {
    INFO("fixture ", name);
    const auto first = test::read_as<ir::Archive>("rar", rar_fixture(name));
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().message.find("'noise.bin' continues in another volume") != std::string::npos);
  }
  for (const char* name : {"rar5_volume.part2.rar", "rar4_volume.part2.rar"}) {
    INFO("fixture ", name);
    const auto second = test::read_as<ir::Archive>("rar", rar_fixture(name));
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().message.find("not the first volume") != std::string::npos);
  }
}

TEST_CASE("archive/rar: unsupported methods and versions are named precisely") {
  const Bytes tree3 = rar_fixture("rar4_tree_m3.rar");
  const auto v20 = test::read_as<ir::Archive>("rar", patch_rar4(tree3, "lorem.txt", 24, 20));
  REQUIRE_FALSE(v20.has_value());
  CHECK(v20.error().message.find("RAR 2.0 compression, which is not supported") != std::string::npos);
  const auto v15 = test::read_as<ir::Archive>("rar", patch_rar4(tree3, "lorem.txt", 24, 15));
  REQUIRE_FALSE(v15.has_value());
  CHECK(v15.error().message.find("RAR 1.5 compression") != std::string::npos);
  // A RAR 5 header asking for an algorithm newer than RAR 7.
  const auto v2 = test::read_as<ir::Archive>("rar", rar5_archive(0, 16, 2u | (1u << 7), Bytes(16)));
  REQUIRE_FALSE(v2.has_value());
  CHECK(v2.error().message.find("algorithm version 2") != std::string::npos);
  // Standard VM filters are recognised by length and CRC-32; anything else is refused.
  CHECK(archive::rar::identify_vm_filter(53, 0xAD576887u) == archive::rar::Unpack3::FilterType::E8);
  CHECK(archive::rar::identify_vm_filter(216, 0xBC85E701u) == archive::rar::Unpack3::FilterType::Audio);
  CHECK(archive::rar::identify_vm_filter(53, 0x12345678u) == archive::rar::Unpack3::FilterType::None);
}

TEST_CASE("archive/rar: dictionaries and sizes are checked against the limits before allocating") {
  // 1 TiB dictionary with an unknown size: refused before any allocation.
  const Bytes huge_dictionary = rar5_archive(0x8, 0, (1u << 7) | (23u << 10), Bytes(64));
  CHECK_STATUS(test::read_as<ir::Archive>("rar", huge_dictionary), Status::Limit);
  // A declared size beyond the limit.
  const Bytes huge_file = rar5_archive(0, std::uint64_t{1} << 40, (1u << 7), Bytes(64));
  CHECK_STATUS(test::read_as<ir::Archive>("rar", huge_file), Status::Limit);
  // Real archives against small limits.
  Limits small;
  small.max_decompressed_bytes = 2000;
  for (const char* name : {"rar5_tree_m3.rar", "rar4_tree_m3.rar", "rar5_tree_solid.rar"}) {
    INFO("fixture ", name);
    CHECK_STATUS(test::read_as<ir::Archive>("rar", rar_fixture(name), "", nullptr, small), Status::Limit);
  }
  Limits few;
  few.max_archive_entries = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("rar", rar_fixture("rar5_tree_m3.rar"), "", nullptr, few), Status::Limit);
}

TEST_CASE("archive/rar: BLAKE2s and BLAKE2sp") {
  archive::rar::Blake2s plain;
  plain.update(to_bytes("abc"));
  const auto abc = plain.finish();
  const std::uint8_t expected[32] = {0x50, 0x8C, 0x5E, 0x8C, 0x32, 0x7C, 0x14, 0xE2, 0xE1, 0xA7, 0x2B,
                                     0xA3, 0x4E, 0xEB, 0x45, 0x2F, 0x37, 0x45, 0x8B, 0x20, 0x9E, 0xD6,
                                     0x3A, 0x29, 0x4D, 0x99, 0x9B, 0x4C, 0x86, 0x67, 0x59, 0x82};
  CHECK(std::equal(abc.begin(), abc.end(), expected));
  // BLAKE2sp does not depend on how the input is split.
  const Bytes data = tree::noise(5000);
  archive::rar::Blake2sp whole;
  whole.update(data);
  archive::rar::Blake2sp pieces;
  for (std::size_t at = 0; at < data.size(); at += 333) pieces.update(ByteView(data).subspan(at, std::min<std::size_t>(333, data.size() - at)));
  CHECK(whole.finish() == pieces.finish());
}

TEST_CASE("archive/rar: detection and conversion") {
  for (const char* name : {"rar5_tree_m3.rar", "rar4_tree_m3.rar"}) {
    INFO("fixture ", name);
    const auto detected = detect_format(Registry::instance(), rar_fixture(name), "");
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == "rar");
    // RAR -> ZIP end to end, then back through the ZIP reader.
    const test::Converted zip = test::convert(rar_fixture(name), "", "zip");
    REQUIRE(zip.ok());
    const auto back = test::read_as<ir::Archive>("zip", zip.data);
    REQUIRE_OK(back);
    tree::check(*back);
  }
  // The extension alone is not enough, and neighbours are not claimed.
  const auto zip = detect_format(Registry::instance(), test::fixture("archive/zip/tree_deflate.zip"), "fake.rar");
  REQUIRE(zip.has_value());
  CHECK(zip->format->id != "rar");
  // RAR cannot be written: a "compress" conversion is refused cleanly.
  const test::Converted same = test::convert(rar_fixture("rar5_tree_m3.rar"), "rar", "rar");
  CHECK_FALSE(same.ok());
}

TEST_CASE("archive/rar: truncated, corrupted and garbage input fails cleanly") {
  for (const char* name : {"rar5_tree_m3.rar", "rar5_tree_solid.rar", "rar5_filters.rar", "rar5_comment.rar",
                           "rar4_tree_m3.rar", "rar4_tree_solid.rar", "rar4_filter_audio.rar", "rar4_filter_rgb.rar",
                           "rar4_comment.rar"}) {
    INFO("fixture ", name);
    test::fuzz_reader<ir::Archive>("rar", rar_fixture(name), 256);
  }
  CHECK_STATUS(test::read_as<ir::Archive>("rar", to_bytes("Rar!\x1A\x07\x01")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Archive>("rar", to_bytes("not a rar archive at all")), Status::Decode);
}

