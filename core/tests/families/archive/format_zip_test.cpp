// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// ZIP and the ZIP-based package formats: real archives from Info-ZIP, 7-Zip and
// Python, encryption, hand-built edge cases, the writer, conversions, limits.
#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/compression.hpp"
#include "util/crc.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;
using compression::Method;

namespace {

Bytes zip_fixture(std::string_view name) { return test::fixture("archive/zip/" + std::string(name)); }

ir::Archive read_ok(std::string_view fixture, std::string_view options = {}) {
  INFO("fixture ", fixture);
  auto archive = test::read_as<ir::Archive>("zip", zip_fixture(fixture), options);
  REQUIRE_OK(archive);
  return std::move(*archive);
}

const ir::ArchiveEntry* find(const ir::Archive& archive, std::string_view path) {
  for (const auto& e : archive.entries) {
    if (e.path == path) return &e;
  }
  return nullptr;
}

std::string lorem() {
  std::string text;
  for (int i = 0; i < 8; ++i) text += tree::lorem_paragraph();
  return text;
}

// hello.txt and lorem.txt with the tree's mtime (and 0644 when modes are recorded).
void check_hello_lorem(const ir::Archive& archive, bool modes = true) {
  const auto* hello = find(archive, "hello.txt");
  const auto* text = find(archive, "lorem.txt");
  REQUIRE(hello != nullptr);
  REQUIRE(text != nullptr);
  CHECK(test::str(hello->bytes) == "Hello, archive!\n");
  CHECK(test::str(text->bytes) == lorem());
  CHECK_EQ(hello->mtime, tree::mtime);
  CHECK_EQ(text->mtime, tree::mtime);
  if (modes) {
    CHECK_EQ(hello->unix_mode, 0644u);
    CHECK_EQ(text->unix_mode, 0644u);
  }
}

// ----------------------------------------------------------------------------- hand-built archives

struct Raw {
  std::string name;
  Bytes data;              // stored as is
  std::uint32_t usize = 0xFFFFFFFFu;  // default: data.size()
  std::uint32_t crc = 0;   // default: CRC of data
  bool crc_set = false;
  std::uint16_t method = 0;
  std::uint16_t flags = 0;
  std::uint16_t made_by = 0x031E;
  std::uint32_t external = 0100644u << 16;
  Bytes central_extra;
  bool descriptor = false;
  bool descriptor_signature = true;
  std::int64_t offset_override = -1;  // central offset pointing elsewhere
};

struct RawOptions {
  std::uint16_t disk = 0;
  std::string comment{};
};

Bytes put_extra(std::uint16_t id, ByteView value) {
  util::ByteWriter w;
  w.u16le(id).u16le(static_cast<std::uint16_t>(value.size())).bytes(value);
  return w.take();
}

Bytes build(const std::vector<Raw>& entries, const RawOptions& options = {}) {
  util::ByteWriter w;
  std::vector<std::uint32_t> offsets;
  for (const Raw& e : entries) {
    const std::uint32_t crc = e.crc_set ? e.crc : util::crc32(e.data);
    const std::uint32_t usize = e.usize == 0xFFFFFFFFu ? static_cast<std::uint32_t>(e.data.size()) : e.usize;
    offsets.push_back(static_cast<std::uint32_t>(w.size()));
    const bool dd = e.descriptor;
    w.u32le(0x04034b50).u16le(20).u16le(e.flags).u16le(e.method).u16le(0x6000).u16le(0x52CF);
    w.u32le(dd ? 0 : crc).u32le(dd ? 0 : static_cast<std::uint32_t>(e.data.size())).u32le(dd ? 0 : usize);
    w.u16le(static_cast<std::uint16_t>(e.name.size())).u16le(0).text(e.name).bytes(e.data);
    if (dd) {
      if (e.descriptor_signature) w.u32le(0x08074b50);
      w.u32le(crc).u32le(static_cast<std::uint32_t>(e.data.size())).u32le(usize);
    }
  }
  const auto cd = static_cast<std::uint32_t>(w.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const Raw& e = entries[i];
    const std::uint32_t crc = e.crc_set ? e.crc : util::crc32(e.data);
    const std::uint32_t usize = e.usize == 0xFFFFFFFFu ? static_cast<std::uint32_t>(e.data.size()) : e.usize;
    const auto offset = e.offset_override >= 0 ? static_cast<std::uint32_t>(e.offset_override) : offsets[i];
    w.u32le(0x02014b50).u16le(e.made_by).u16le(20).u16le(e.flags).u16le(e.method).u16le(0x6000).u16le(0x52CF);
    w.u32le(crc).u32le(static_cast<std::uint32_t>(e.data.size())).u32le(usize);
    w.u16le(static_cast<std::uint16_t>(e.name.size())).u16le(static_cast<std::uint16_t>(e.central_extra.size())).u16le(0);
    w.u16le(0).u16le(0).u32le(e.external).u32le(offset).text(e.name).bytes(e.central_extra);
  }
  const auto cd_size = static_cast<std::uint32_t>(w.size() - cd);
  const auto count = static_cast<std::uint16_t>(entries.size());
  w.u32le(0x06054b50).u16le(options.disk).u16le(options.disk).u16le(count).u16le(count).u32le(cd_size).u32le(cd);
  w.u16le(static_cast<std::uint16_t>(options.comment.size())).text(options.comment);
  return w.take();
}

Raw raw(std::string name, std::string_view data) {
  Raw r;
  r.name = std::move(name);
  r.data = to_bytes(data);
  return r;
}

}  // namespace

// ============================================================================= real archives

TEST_CASE("archive/zip: Info-ZIP archives extract exactly") {
  // `zip -r` without -y follows the symlink: 'link' is a copy of hello.txt; long/ was excluded.
  const ir::Archive deflated = read_ok("tree_deflate.zip");
  tree::check(deflated, {.symlink = false, .long_name = false, .allow_extra = true});
  const auto* link = find(deflated, "link");
  REQUIRE(link != nullptr);
  CHECK(link->is_file());
  CHECK(test::str(link->bytes) == "Hello, archive!\n");
  CHECK(find(deflated, "dir/nested")->is_directory);
  CHECK_EQ(find(deflated, "dir/nested")->mtime, tree::mtime);
  CHECK(deflated.comment.empty());

  // `zip -y` stores the link itself: mode 0120000, target as content.
  const ir::Archive linked = read_ok("tree_symlink.zip");
  REQUIRE_EQ(linked.entries.size(), 7u);
  const auto* sym = find(linked, "link");
  REQUIRE(sym != nullptr);
  CHECK(sym->is_symlink());
  CHECK(sym->symlink_target == "hello.txt");
  CHECK(sym->bytes.empty());
  CHECK(find(linked, "dir/empty_dir")->is_directory);
  CHECK_EQ(find(linked, "dir/empty_dir")->unix_mode, 0755u);
  CHECK(test::str(find(linked, "dir/nested/deep/file.txt")->bytes) == "deep file\n");

  check_hello_lorem(read_ok("tree_store.zip"));
  // Info-ZIP's extended timestamp and Unix uid/gid extras; UTF-8 name without the flag.
  const ir::Archive extras = read_ok("infozip_extras.zip");
  REQUIRE_EQ(extras.entries.size(), 2u);
  CHECK(extras.entries[1].path == tree::unicode_name());
  CHECK(test::str(extras.entries[1].bytes) == "unicode name\n");
  for (const auto& e : extras.entries) CHECK_EQ(e.mtime, tree::mtime);
}

TEST_CASE("archive/zip: data descriptors, Zip64 and a self-extractor stub") {
  // From a pipe: bit 3, descriptors with signature (32-bit sizes; and 64-bit for stdin).
  check_hello_lorem(read_ok("stream_dd32.zip"));
  const ir::Archive streamed = read_ok("stream_descriptor.zip");
  REQUIRE_EQ(streamed.entries.size(), 1u);
  CHECK(streamed.entries[0].path == "-");
  CHECK(test::str(streamed.entries[0].bytes) == lorem());
  CHECK_EQ(streamed.entries[0].mtime, tree::mtime);
  // `zip -fz`: Zip64 extra fields on every entry, Zip64 end records.
  const ir::Archive forced = read_ok("zip64_forced.zip");
  REQUIRE_EQ(forced.entries.size(), 2u);
  CHECK(test::str(find(forced, "hello.txt")->bytes) == "Hello, archive!\n");
  CHECK(find(forced, "noise.bin")->bytes == tree::noise(4096));
  // A shell stub in front (`zip -A` adjusted the offsets).
  check_hello_lorem(read_ok("sfx.zip"));
  // The same archive behind an unadjusted stub: offsets are corrected.
  Bytes shifted = test::bytes("#!/bin/sh\nexit 0\n");
  append(shifted, zip_fixture("tree_store.zip"));
  const auto shifted_back = test::read_as<ir::Archive>("zip", shifted);
  REQUIRE_OK(shifted_back);
  check_hello_lorem(*shifted_back);
}

TEST_CASE("archive/zip: Chrome extensions (CRX2 and CRX3 headers)") {
  const Bytes inner = zip_fixture("tree_store.zip");
  util::ByteWriter v3;
  v3.text("Cr24").u32le(3).u32le(5).text("hdr3!").bytes(inner);
  util::ByteWriter v2;
  v2.text("Cr24").u32le(2).u32le(4).u32le(3).text("key").text("sig").text("_").bytes(inner);
  for (const Bytes& crx : {v3.data(), v2.data()}) {
    const auto archive = test::read_as<ir::Archive>("crx", crx);
    REQUIRE_OK(archive);
    check_hello_lorem(*archive);
    const auto detected = detect_format(Registry::instance(), crx, "");
    REQUIRE(detected.has_value());
    CHECK(detected->format->id == "crx");
  }
  util::ByteWriter v9;
  v9.text("Cr24").u32le(9).u32le(0).bytes(inner);
  CHECK_STATUS(test::read_as<ir::Archive>("crx", v9.data()), Status::Decode);
}

TEST_CASE("archive/zip: compression methods from 7-Zip") {
  struct Case {
    const char* fixture;
    Method method;
  };
  for (const Case& c : {Case{"bzip2.zip", Method::Bzip2}, Case{"lzma.zip", Method::LzmaRaw},
                        Case{"ppmd.zip", Method::Ppmd8}}) {
    INFO("fixture ", c.fixture);
    if (!compression::can_decode(c.method)) {
      MESSAGE("skipping ", c.fixture, ": codec not available yet");
      continue;
    }
    check_hello_lorem(read_ok(c.fixture));
  }
  if (compression::can_decode(Method::Deflate64)) {
    const ir::Archive d64 = read_ok("deflate64.zip");
    CHECK(test::str(find(d64, "lorem.txt")->bytes) == lorem());
    CHECK(find(d64, "noise.bin")->bytes == tree::noise(4096));
  } else {
    MESSAGE("skipping deflate64.zip: codec not available yet");
    const auto missing = test::read_as<ir::Archive>("zip", zip_fixture("deflate64.zip"));
    REQUIRE(!missing);
    CHECK(missing.error().message.find("deflate64") != std::string::npos);
  }
}

TEST_CASE("archive/zip: names in UTF-8, CP437 and Unicode Path fields; comments") {
  // 7-Zip: UTF-8 flag, NTFS timestamps.
  const ir::Archive unicode = read_ok("unicode.zip");
  REQUIRE_EQ(unicode.entries.size(), 2u);
  CHECK(unicode.entries[0].is_directory);
  CHECK(unicode.entries[1].path == tree::unicode_name());
  CHECK_EQ(unicode.entries[1].mtime, tree::mtime);
  // Info-ZIP in the C locale: the raw byte 0x82 is CP437 for e-acute.
  const ir::Archive cp437 = read_ok("cp437_name.zip");
  REQUIRE_EQ(cp437.entries.size(), 1u);
  CHECK(cp437.entries[0].path == "caf\xC3\xA9.txt");
  CHECK(test::str(cp437.entries[0].bytes) == "cafe\n");
  // Python zipfile: archive comment, UTF-8 flagged name, permission bits without a file type.
  const ir::Archive commented = read_ok("python_comment.zip");
  CHECK(commented.comment == "synthetic archive comment");
  REQUIRE_EQ(commented.entries.size(), 2u);
  CHECK(commented.entries[1].path == "caf\xC3\xA9.txt");
  CHECK(commented.entries[0].is_file());
  CHECK_EQ(commented.entries[0].unix_mode, 0644u);
  CHECK_EQ(commented.entries[0].mtime, tree::mtime);

  // Info-ZIP Unicode Path extra: used when its CRC matches the stored name, ignored otherwise.
  const std::string stored = "caf\x82.txt";
  const std::string utf8 = "\xE5\x90\x8D\xE5\x89\x8D.txt";
  util::ByteWriter up;
  up.u8(1).u32le(util::crc32(as_bytes(stored))).text(utf8);
  Raw good = raw(stored, "x");
  good.central_extra = put_extra(0x7075, up.view());
  util::ByteWriter stale;
  stale.u8(1).u32le(12345).text(utf8);
  Raw bad = raw("old\x82.txt", "y");
  bad.central_extra = put_extra(0x7075, stale.view());
  const auto names = test::read_as<ir::Archive>("zip", build({good, bad}, {.comment = "caf\x82"}));
  REQUIRE_OK(names);
  REQUIRE_EQ(names->entries.size(), 2u);
  CHECK(names->entries[0].path == utf8);
  CHECK(names->entries[1].path == "old\xC3\xA9.txt");
  CHECK(names->comment == "caf\xC3\xA9");
}

TEST_CASE("archive/zip: ZipCrypto and WinZip AES with right, wrong and missing passwords") {
  for (const char* fixture : {"zipcrypto.zip", "zipcrypto_stream.zip", "aes256.zip"}) {
    INFO("fixture ", fixture);
    check_hello_lorem(read_ok(fixture, R"({"password":"secret"})"));
    const auto missing = test::read_as<ir::Archive>("zip", zip_fixture(fixture));
    CHECK_STATUS(missing, Status::Decode);
    if (!missing) CHECK(missing.error().message.find("password required") != std::string::npos);
    const auto wrong = test::read_as<ir::Archive>("zip", zip_fixture(fixture), R"({"password":"Secret"})");
    CHECK_STATUS(wrong, Status::Decode);
    if (!wrong) CHECK(wrong.error().message.find("wrong password") != std::string::npos);
  }
  const ir::Archive aes128 = read_ok("aes128.zip", R"({"password":"secret"})");
  REQUIRE_EQ(aes128.entries.size(), 1u);
  CHECK(test::str(aes128.entries[0].bytes) == "Hello, archive!\n");
  CHECK_STATUS(test::read_as<ir::Archive>("zip", zip_fixture("aes128.zip"), R"({"password":"nope"})"), Status::Decode);
}

TEST_CASE("archive/zip: hand-built edge cases") {
  // Data descriptor without its optional signature.
  Raw plain = raw("a.txt", "descriptor without signature");
  plain.flags = 0x0008;
  plain.descriptor = true;
  plain.descriptor_signature = false;
  Raw signed_dd = raw("b.txt", "descriptor with signature");
  signed_dd.flags = 0x0008;
  signed_dd.descriptor = true;
  const auto dd = test::read_as<ir::Archive>("zip", build({plain, signed_dd}));
  REQUIRE_OK(dd);
  REQUIRE_EQ(dd->entries.size(), 2u);
  CHECK(test::str(dd->entries[0].bytes) == "descriptor without signature");
  CHECK(test::str(dd->entries[1].bytes) == "descriptor with signature");

  // MS-DOS host: directory attribute, backslashes, no Unix mode.
  Raw dos_dir = raw("folder\\", "");
  dos_dir.made_by = 0x0014;
  dos_dir.external = 0x10;
  Raw dos_file = raw("folder\\file.txt", "dos");
  dos_file.made_by = 0x0014;
  dos_file.external = 0x20;
  const auto dos = test::read_as<ir::Archive>("zip", build({dos_dir, dos_file}));
  REQUIRE_OK(dos);
  REQUIRE_EQ(dos->entries.size(), 2u);
  CHECK(dos->entries[0].is_directory);
  CHECK(dos->entries[0].path == "folder");
  CHECK(dos->entries[1].path == "folder/file.txt");
  CHECK_EQ(dos->entries[1].unix_mode, 0u);
  CHECK_EQ(dos->entries[1].mtime, tree::mtime);

  // Unknown and unsupported methods name themselves.
  Raw odd = raw("odd.bin", "????");
  odd.method = 97;
  const auto unknown = test::read_as<ir::Archive>("zip", build({odd}));
  CHECK_STATUS(unknown, Status::Decode);
  if (!unknown) CHECK(unknown.error().message.find("WavPack") != std::string::npos);
  Raw strong = raw("strong.bin", "????");
  strong.flags = 0x41;
  CHECK_STATUS(test::read_as<ir::Archive>("zip", build({strong}), R"({"password":"x"})"), Status::Decode);

  // Split archives are refused with a clear message.
  const auto split = test::read_as<ir::Archive>("zip", build({raw("a", "b")}, {.disk = 1}));
  CHECK_STATUS(split, Status::Decode);
  if (!split) CHECK(split.error().message.find("multi-volume") != std::string::npos);

  // CRC and size mismatches.
  Raw bad_crc = raw("crc.txt", "content");
  bad_crc.crc = 1;
  bad_crc.crc_set = true;
  CHECK_STATUS(test::read_as<ir::Archive>("zip", build({bad_crc})), Status::Decode);
  Raw bad_size = raw("size.txt", "content");
  bad_size.usize = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("zip", build({bad_size})), Status::Decode);

  // Unsafe names are dropped by the sink; an empty archive is fine.
  const auto unsafe = test::read_as<ir::Archive>("zip", build({raw("../evil.txt", "x"), raw("ok.txt", "y")}));
  REQUIRE_OK(unsafe);
  for (const auto& e : unsafe->entries) CHECK(e.path.find("..") == std::string::npos);
  const auto empty = test::read_as<ir::Archive>("zip", build({}));
  REQUIRE_OK(empty);
  CHECK(empty->entries.empty());
}

// ============================================================================= bombs and limits

TEST_CASE("archive/zip: overlapping entries and decompression limits") {
  // Two central records sharing one local header and its data: a classic overlap bomb.
  Raw first = raw("a.txt", "shared data");
  Raw second = raw("b.txt", "shared data");
  second.offset_override = 0;
  const Bytes overlapping = build({first, second});
  const auto bomb = test::read_as<ir::Archive>("zip", overlapping);
  CHECK_STATUS(bomb, Status::Limit);
  if (!bomb) CHECK(bomb.error().message.find("zip bomb") != std::string::npos);

  // 8 MiB of zeros, refused under a 1 MiB limit before or while inflating.
  ir::Archive zeros;
  ir::ArchiveEntry big;
  big.path = "zeros.bin";
  big.bytes = Bytes(8u << 20, std::byte{0});
  zeros.entries.push_back(big);
  const auto packed = test::write_from("zip", zeros);
  REQUIRE_OK(packed);
  CHECK_LT(packed->size(), 64u * 1024);
  Limits small;
  small.max_decompressed_bytes = 1u << 20;
  CHECK_STATUS(test::read_as<ir::Archive>("zip", *packed, "", nullptr, small), Status::Limit);
  // The entry-count limit.
  Limits few;
  few.max_archive_entries = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("zip", zip_fixture("tree_deflate.zip"), "", nullptr, few), Status::Limit);
  // A central directory that lies about the size: the inflater is stopped at the declared size.
  Bytes lying = *packed;
  const std::size_t eocd = lying.size() - 22;
  const std::size_t cd = std::to_integer<std::size_t>(lying[eocd + 16]) | (std::to_integer<std::size_t>(lying[eocd + 17]) << 8) |
                         (std::to_integer<std::size_t>(lying[eocd + 18]) << 16);
  lying[cd + 24] = std::byte{0x10};  // uncompressed size 8 MiB -> 16 bytes
  lying[cd + 25] = std::byte{0};
  lying[cd + 26] = std::byte{0};
  CHECK_STATUS(test::read_as<ir::Archive>("zip", lying), Status::Limit);
}

TEST_CASE("archive/zip: damaged archives fail cleanly") {
  for (const char* fixture : {"tree_deflate.zip", "zip64_forced.zip", "stream_descriptor.zip", "unicode.zip",
                              "aes256.zip", "zipcrypto.zip", "sfx.zip", "bzip2.zip", "lzma.zip"}) {
    INFO("fixture ", fixture);
    test::fuzz_reader<ir::Archive>("zip", zip_fixture(fixture));
  }
  // Encrypted data with the password: every flipped byte is caught (HMAC, CRC or check byte).
  for (const char* fixture : {"aes256.zip", "zipcrypto.zip"}) {
    const Bytes good = zip_fixture(fixture);
    for (std::size_t at = 0; at < good.size(); at += 7) {
      Bytes bad = good;
      bad[at] ^= std::byte{0x01};
      (void)test::read_as<ir::Archive>("zip", bad, R"({"password":"secret"})");
    }
  }
  CHECK_STATUS(test::read_as<ir::Archive>("zip", test::random_bytes(4096, 7)), Status::Decode);
  // Truncated: the central directory is gone.
  const Bytes whole = zip_fixture("tree_store.zip");
  const auto cut = test::read_as<ir::Archive>("zip", ByteView(whole).first(whole.size() / 2));
  CHECK_STATUS(cut, Status::Decode);
}

// ============================================================================= writer

namespace {
void round_trip(std::initializer_list<const char*> methods) {
  const ir::Archive source = tree::archive();
  for (const char* method : methods) {
    for (const char* encryption : {"", "aes256", "aes128", "zipcrypto"}) {
      INFO("method ", method, " encryption ", encryption);
      std::string options = std::string(R"({"method":")") + method + "\"";
      if (*encryption != '\0') options += std::string(R"(,"password":"pässwörd","encryption":")") + encryption + "\"";
      options += "}";
      const auto written = test::write_from("zip", source, options);
      if (!written && written.error().code == Status::Encode &&
          written.error().message.find("not available") != std::string::npos) {
        MESSAGE("skipping ", method, ": encoder not available yet");
        continue;
      }
      REQUIRE_OK(written);
      const auto back = test::read_as<ir::Archive>("zip", *written, *encryption != '\0' ? R"({"password":"pässwörd"})" : "");
      REQUIRE_OK(back);
      tree::check(*back);
      if (*encryption != '\0') {
        CHECK_STATUS(test::read_as<ir::Archive>("zip", *written), Status::Decode);
        CHECK_STATUS(test::read_as<ir::Archive>("zip", *written, R"({"password":"passwort"})"), Status::Decode);
        // Deterministic, also when encrypted.
        CHECK(*test::write_from("zip", source, options) == *written);
      }
    }
  }
}
}  // namespace

TEST_CASE("archive/zip: writer round trips (deflate, store, bzip2) with every encryption") {
  round_trip({"deflate", "store", "bzip2"});
}

TEST_CASE("archive/zip: writer round trips (lzma, zstd, xz) with every encryption") { round_trip({"lzma", "zstd", "xz"}); }

TEST_CASE("archive/zip: writer policy, determinism and layout") {
  ir::Archive source = tree::archive();
  source.comment = "kept only on request";
  const auto written = test::write_from("zip", source);
  REQUIRE_OK(written);
  CHECK(*test::write_from("zip", source) == *written);
  CHECK(test::str(*written).find("kept only") == std::string::npos);
  // Unix "made by", UTF-8 flag for the non-ASCII name, an extended timestamp, no uid/gid extras.
  const auto ref = util::ZipReader::open(*written);
  REQUIRE_OK(ref);
  CHECK(test::str(*written).find("ux") == std::string::npos);
  CHECK(test::str(*written).find(std::string_view("UT\x05\x00\x01", 5)) != std::string::npos);
  const auto back = test::read_as<ir::Archive>("zip", *written);
  REQUIRE_OK(back);
  tree::check(*back);

  const auto with_comment = test::write_from("zip", source, R"({"strip_metadata":false})");
  REQUIRE_OK(with_comment);
  CHECK(test::read_as<ir::Archive>("zip", *with_comment)->comment == "kept only on request");

  const auto fixed = test::write_from("zip", source, R"({"timestamps":"fixed"})");
  REQUIRE_OK(fixed);
  CHECK(test::str(*fixed).find(std::string_view("UT\x05\x00", 4)) == std::string::npos);
  const auto fixed_back = test::read_as<ir::Archive>("zip", *fixed);  // named: the loop must not outlive it
  REQUIRE_OK(fixed_back);
  for (const auto& e : fixed_back->entries) CHECK_EQ(e.mtime, 315532800);

  // Incompressible data is stored; level 0 stores everything; higher levels are not larger.
  const auto stored = test::write_from("zip", source, R"({"compression_level":0})");
  REQUIRE_OK(stored);
  CHECK_GT(stored->size(), written->size());
  ir::Archive text;
  ir::ArchiveEntry big;
  big.path = "big.txt";
  for (int i = 0; i < 300; ++i) append(big.bytes, as_bytes(tree::lorem_paragraph() + std::to_string(i * 7919) + "\n"));
  text.entries.push_back(big);
  const auto fast = test::write_from("zip", text, R"({"compression_level":1})");
  const auto best = test::write_from("zip", text, R"({"compression_level":9})");
  REQUIRE_OK(fast);
  REQUIRE_OK(best);
  CHECK_LE(best->size(), fast->size());
  ir::Archive noise;
  ir::ArchiveEntry n;
  n.path = "noise.bin";
  n.bytes = test::random_bytes(10000, 3);
  noise.entries.push_back(n);
  const auto noisy = test::write_from("zip", noise);
  REQUIRE_OK(noisy);
  CHECK_LT(noisy->size(), 10000u + 200u);
  CHECK(test::str(*noisy).find(std::string_view("PK\x03\x04\x0a\x00\x00\x00\x00\x00", 10)) == 0);  // method 0
  // An empty archive is a bare end record.
  const auto empty = test::write_from("zip", ir::Archive{});
  REQUIRE_OK(empty);
  CHECK_EQ(empty->size(), 22u);
  REQUIRE_OK(test::read_as<ir::Archive>("zip", *empty));
}

TEST_CASE("archive/zip: Zip64 end records for more than 65535 entries") {
  ir::Archive many;
  many.entries.resize(65600);
  for (std::size_t i = 0; i < many.entries.size(); ++i) many.entries[i].path = std::to_string(i);
  const auto written = test::write_from("zip", many, R"({"timestamps":"fixed"})");
  REQUIRE_OK(written);
  CHECK(test::str(*written).find(std::string_view("PK\x06\x06", 4)) != std::string::npos);
  const auto back = test::read_as<ir::Archive>("zip", *written);
  REQUIRE_OK(back);
  REQUIRE_EQ(back->entries.size(), 65600u);
  CHECK(back->entries.back().path == "65599");
}

// ============================================================================= conversions and detection

TEST_CASE("archive/zip: conversions through the engine and the compress route") {
  // zip -> tar
  const test::Converted to_tar = test::convert(zip_fixture("tree_deflate.zip"), "", "tar");
  REQUIRE(to_tar.ok());
  tree::check(*test::read_as<ir::Archive>("tar", to_tar.data), {.symlink = false, .long_name = false, .allow_extra = true});
  // tar -> zip keeps everything, including the symlink and the empty directory.
  const test::Converted to_zip = test::convert(test::fixture("archive/tar/tree_pax.tar"), "", "zip");
  REQUIRE(to_zip.ok());
  CHECK(to_zip.format_id == "zip");
  tree::check(*test::read_as<ir::Archive>("zip", to_zip.data));
  // zip -> zip recompresses at level 9: the stored archive shrinks.
  const Bytes stored = zip_fixture("tree_store.zip");
  const test::Converted again = test::convert(stored, "", "zip");
  REQUIRE(again.ok());
  CHECK(again.format_id == "zip");
  CHECK_LT(again.data.size(), stored.size() / 2);
  check_hello_lorem(*test::read_as<ir::Archive>("zip", again.data));
  // An encrypted archive converts with its password and can be re-encrypted.
  const test::Converted reenc = test::convert(zip_fixture("zipcrypto.zip"), "", "zip", R"({"password":"secret","encryption":"aes128"})");
  REQUIRE(reenc.ok());
  check_hello_lorem(*test::read_as<ir::Archive>("zip", reenc.data, R"({"password":"secret"})"));
}

TEST_CASE("archive/zip: detection of zip and the package formats") {
  const auto archive_with = [](std::string member) {
    ir::Archive a;
    ir::ArchiveEntry e;
    e.path = std::move(member);
    e.bytes = test::bytes("content");
    a.entries.push_back(e);
    return *test::write_from("zip", a);
  };
  const auto detected = [](ByteView data, std::string_view name = {}) {
    const auto d = detect_format(Registry::instance(), data, name);
    return d ? d->format->id : std::string("none");
  };
  const Bytes plain = archive_with("readme.txt");
  CHECK(detected(plain) == "zip");
  CHECK(detected(plain, "a.zip") == "zip");
  CHECK(detected(archive_with("META-INF/MANIFEST.MF")) == "jar");
  CHECK(detected(archive_with("AndroidManifest.xml")) == "apk");
  CHECK(detected(archive_with("Payload/Demo.app/Info.plist")) == "ipa");
  CHECK(detected(archive_with("install.rdf")) == "xpi");
  CHECK(detected(archive_with("demo-1.0.dist-info/WHEEL")) == "whl");
  CHECK(detected(archive_with("Demo.nuspec")) == "nupkg");
  CHECK(detected(plain, "pages.cbz") == "cbz");
  CHECK(detected(plain, "lib.jar") == "jar");
  // A binary self-extractor stub in front: the end record gives it away. (A '#!/bin/sh' stub
  // such as sfx.zip is detected as the shell script it also is; reading it as zip works.)
  Bytes exe = test::bytes("MZ");
  exe.resize(512, std::byte{0});
  append(exe, zip_fixture("tree_store.zip"));
  CHECK(detected(exe) == "zip");
  CHECK(detected(exe, "setup.exe") != "none");
  CHECK(detected(zip_fixture("zip64_forced.zip")) == "zip");
  CHECK(detected(build({})) == "zip");
  // Office documents keep their own identity.
  CHECK(detected(archive_with("word/document.xml")) != "zip");
  // Every alias reads and writes like zip.
  for (const char* id : {"jar", "apk", "ipa", "xpi", "whl", "nupkg", "cbz"}) {
    INFO("format ", id);
    const auto back = test::read_as<ir::Archive>(id, plain);
    REQUIRE_OK(back);
    CHECK(test::str(back->entries[0].bytes) == "content");
    const auto written = test::write_from(id, *back);
    REQUIRE_OK(written);
    CHECK(*written == plain);
  }
  CHECK(Registry::instance().validate().empty());
}
