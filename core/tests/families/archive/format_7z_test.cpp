// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// 7z: real archives from 7-Zip (LZMA, LZMA2, PPMd, BZip2, Deflate, Copy, BCJ2,
// ARM64, AES with and without encrypted headers), hand-built edge cases, the
// writer (every method, solid or not, encryption), conversions, limits and
// damaged input.
#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/compression.hpp"
#include "families/archive/formats/sevenzip_internal.hpp"
#include "util/crc.hpp"
#include "util/text.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;
namespace sz = cvt::archive::sevenzip;
using compression::Method;

namespace {

Bytes fixture7z(std::string_view name) { return test::fixture("archive/7z/" + std::string(name)); }

ir::Archive read_ok(std::string_view fixture, std::string_view options = {}) {
  INFO("fixture ", fixture);
  auto archive = test::read_as<ir::Archive>("7z", fixture7z(fixture), options);
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

// The BCJ2 / ARM64 fixtures pack mixed.bin = lorem.txt followed by noise.bin.
Bytes mixed() {
  Bytes out = to_bytes(lorem());
  append(out, tree::noise(4096));
  return out;
}

void check_hello_lorem(const ir::Archive& archive) {
  const auto* hello = find(archive, "hello.txt");
  const auto* text = find(archive, "lorem.txt");
  REQUIRE(hello != nullptr);
  REQUIRE(text != nullptr);
  CHECK(test::str(hello->bytes) == "Hello, archive!\n");
  CHECK(test::str(text->bytes) == lorem());
  CHECK_EQ(hello->mtime, tree::mtime);
  CHECK_EQ(text->mtime, tree::mtime);
  CHECK_EQ(hello->unix_mode, 0644u);
  CHECK_EQ(text->unix_mode, 0644u);
}

bool contains(ByteView haystack, ByteView needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

// ----------------------------------------------------------------------------- hand-built archives

struct RawFile {
  std::string name;
  Bytes data;                 // empty + !directory = empty file
  bool directory = false;
  bool anti = false;
  std::optional<std::uint32_t> attrib;
};

struct RawOptions {
  sz::Folder folder{};        // coders for the single folder; default Copy
  Bytes packed{};             // overrides the packed data (default: the files' data joined)
  std::optional<std::uint64_t> unpack_size{};  // overrides the folder's declared size
  bool dummy = true;          // add a kDummy record
  bool bad_crc = false;
  bool encode_header = false;  // wrap the header in a Copy-coded kEncodedHeader
};

Bytes build(const std::vector<RawFile>& files, RawOptions o = {}) {
  util::ByteWriter body;
  Bytes joined;
  std::vector<const RawFile*> streams;
  for (const RawFile& f : files) {
    if (!f.data.empty()) {
      append(joined, f.data);
      streams.push_back(&f);
    }
  }
  if (o.folder.coders.empty()) {
    o.folder.coders.push_back(sz::Coder{});
    o.folder.packed = {0};
  }
  const Bytes packed = o.packed.empty() ? joined : o.packed;
  if (o.folder.unpack_sizes.empty()) o.folder.unpack_sizes = {o.unpack_size.value_or(joined.size())};

  util::ByteWriter h;
  h.u8(sz::kHeader);
  if (!streams.empty()) {
    h.u8(sz::kMainStreamsInfo);
    h.u8(sz::kPackInfo).u8(0);
    sz::write_number(h, 1);
    h.u8(sz::kSize);
    sz::write_number(h, packed.size());
    h.u8(sz::kEnd);
    h.u8(sz::kUnpackInfo).u8(sz::kFolder).u8(1).u8(0);
    sz::write_folder(h, o.folder);
    h.u8(sz::kCodersUnpackSize);
    for (const std::uint64_t s : o.folder.unpack_sizes) sz::write_number(h, s);
    h.u8(sz::kEnd);
    h.u8(sz::kSubStreamsInfo).u8(sz::kNumUnpackStream);
    sz::write_number(h, streams.size());
    h.u8(sz::kSize);
    for (std::size_t i = 0; i + 1 < streams.size(); ++i) sz::write_number(h, streams[i]->data.size());
    h.u8(sz::kCrc).u8(1);
    for (const RawFile* f : streams) h.u32le(util::crc32(f->data) ^ (o.bad_crc ? 1u : 0u));
    h.u8(sz::kEnd).u8(sz::kEnd);
  }
  h.u8(sz::kFilesInfo);
  sz::write_number(h, files.size());
  const auto bits = [](const std::vector<bool>& v) {
    util::ByteWriter w;
    for (std::size_t i = 0; i < v.size(); i += 8) {
      std::uint8_t b = 0;
      for (std::size_t k = 0; k < 8 && i + k < v.size(); ++k) {
        if (v[i + k]) b = static_cast<std::uint8_t>(b | (0x80u >> k));
      }
      w.u8(b);
    }
    return w.take();
  };
  const auto prop = [&](std::uint8_t id, ByteView v) {
    h.u8(id);
    sz::write_number(h, v.size());
    h.bytes(v);
  };
  std::vector<bool> empty_stream;
  std::vector<bool> empty_file;
  std::vector<bool> anti;
  for (const RawFile& f : files) {
    empty_stream.push_back(f.data.empty());
    if (f.data.empty()) {
      empty_file.push_back(!f.directory);
      anti.push_back(f.anti);
    }
  }
  prop(sz::kEmptyStream, bits(empty_stream));
  prop(sz::kEmptyFile, bits(empty_file));
  prop(sz::kAnti, bits(anti));
  if (o.dummy) prop(sz::kDummy, Bytes(3, std::byte{0}));
  util::ByteWriter names;
  names.u8(0);
  for (const RawFile& f : files) {
    for (const char16_t c : util::utf8_to_utf16(f.name)) names.u16le(static_cast<std::uint16_t>(c));
    names.u16le(0);
  }
  prop(sz::kName, names.view());
  util::ByteWriter times;  // all defined
  times.u8(1).u8(0);
  for (std::size_t i = 0; i < files.size(); ++i) times.u64le((static_cast<std::uint64_t>(tree::mtime) + 11644473600ull) * 10000000ull);
  prop(sz::kMTime, times.view());
  prop(sz::kATime, times.view());  // ignored
  util::ByteWriter attrs;  // only some defined
  attrs.u8(0);
  std::vector<bool> have;
  for (const RawFile& f : files) have.push_back(f.attrib.has_value());
  attrs.bytes(bits(have)).u8(0);
  for (const RawFile& f : files) {
    if (f.attrib) attrs.u32le(*f.attrib);
  }
  prop(sz::kWinAttributes, attrs.view());
  h.u8(sz::kEnd).u8(sz::kEnd);

  util::ByteWriter tail;
  Bytes header = h.take();
  Bytes data = packed;
  if (o.encode_header) {
    // A Copy-coded encoded header placed after the data.
    sz::Folder hf;
    hf.coders.push_back(sz::Coder{});
    hf.packed = {0};
    util::ByteWriter enc;
    enc.u8(sz::kEncodedHeader).u8(sz::kPackInfo);
    sz::write_number(enc, data.size());
    enc.u8(1).u8(sz::kSize);
    sz::write_number(enc, header.size());
    enc.u8(sz::kEnd).u8(sz::kUnpackInfo).u8(sz::kFolder).u8(1).u8(0);
    sz::write_folder(enc, hf);
    enc.u8(sz::kCodersUnpackSize);
    sz::write_number(enc, header.size());
    enc.u8(sz::kCrc).u8(1).u32le(util::crc32(header)).u8(sz::kEnd).u8(sz::kEnd);
    append(data, header);
    header = enc.take();
  }
  util::ByteWriter start;
  start.u64le(data.size()).u64le(header.size()).u32le(util::crc32(header));
  util::ByteWriter out;
  out.bytes(std::span<const std::uint8_t>(sz::signature.data(), sz::signature.size())).u8(0).u8(4);
  out.u32le(util::crc32(start.view())).bytes(start.view()).bytes(data).bytes(header);
  return out.take();
}

RawFile raw(std::string name, std::string_view data, std::optional<std::uint32_t> attrib = std::nullopt) {
  RawFile f;
  f.name = std::move(name);
  f.data = to_bytes(data);
  f.attrib = attrib;
  return f;
}

}  // namespace

// ============================================================================= real archives

TEST_CASE("archive/7z: 7-Zip LZMA2 solid archive of the whole tree") {
  // -snl stores 'link' as a symlink (S_IFLNK, target as content); directories carry 0755.
  const ir::Archive archive = read_ok("tree_lzma2.7z");
  tree::check(archive);
  CHECK_EQ(archive.entries.size(), 15u);
  for (const char* dir : {"dir", "dir/nested", "dir/nested/deep", "dir/empty_dir", "unicode", "long"}) {
    INFO("directory ", dir);
    const auto* e = find(archive, dir);
    REQUIRE(e != nullptr);
    CHECK(e->is_directory);
    CHECK_EQ(e->unix_mode, 0755u);
    CHECK_EQ(e->mtime, tree::mtime);
  }
  CHECK(find(archive, "empty.txt")->is_file());
  CHECK(archive.comment.empty());
}

TEST_CASE("archive/7z: non-solid LZMA, BZip2, Deflate, Copy and PPMd archives") {
  // -ms=off -m0=LZMA: one folder per file.
  const ir::Archive lzma = read_ok("tree_lzma.7z");
  check_hello_lorem(lzma);
  CHECK(find(lzma, "noise.bin")->bytes == tree::noise(4096));
  CHECK(test::str(find(lzma, "dir/nested/deep/file.txt")->bytes) == "deep file\n");
  CHECK(find(lzma, "dir/empty_dir")->is_directory);
  CHECK_EQ(find(lzma, "dir/empty_dir")->unix_mode, 0755u);

  check_hello_lorem(read_ok("bzip2.7z"));
  if (compression::can_decode(Method::Deflate)) check_hello_lorem(read_ok("deflate.7z"));
  // -mhc=off: a plain (unencoded) header; an empty file.
  const ir::Archive copy = read_ok("copy.7z");
  check_hello_lorem(copy);
  REQUIRE(find(copy, "empty.txt") != nullptr);
  CHECK(find(copy, "empty.txt")->is_file());
  CHECK(find(copy, "empty.txt")->bytes.empty());

  if (compression::can_decode(Method::Ppmd7)) {
    check_hello_lorem(read_ok("ppmd.7z"));
  } else {
    MESSAGE("skipping ppmd.7z: PPMd decoder not available yet");
    const auto missing = test::read_as<ir::Archive>("7z", fixture7z("ppmd.7z"));
    REQUIRE(!missing);
    CHECK(missing.error().message.find("PPMd") != std::string::npos);
  }
}

TEST_CASE("archive/7z: BCJ2 (four-stream coder graph), ARM64 and Delta filters") {
  for (const char* fixture : {"bcj2.7z", "arm64.7z", "delta.7z"}) {
    INFO("fixture ", fixture);
    const ir::Archive archive = read_ok(fixture);
    REQUIRE_EQ(archive.entries.size(), 1u);
    CHECK(archive.entries[0].path == "mixed.bin");
    CHECK(archive.entries[0].bytes == mixed());
    CHECK_EQ(archive.entries[0].mtime, 1623758400);
  }
}

TEST_CASE("archive/7z: AES-256 with right, wrong and missing passwords") {
  // -mhe=on: the header is encrypted too.
  check_hello_lorem(read_ok("encrypted.7z", R"({"password":"secret"})"));
  const ir::Archive data_only = read_ok("encrypted_data.7z", R"({"password":"secret"})");
  REQUIRE_EQ(data_only.entries.size(), 1u);
  CHECK(test::str(data_only.entries[0].bytes) == "Hello, archive!\n");
  for (const char* fixture : {"encrypted.7z", "encrypted_data.7z"}) {
    INFO("fixture ", fixture);
    const auto missing = test::read_as<ir::Archive>("7z", fixture7z(fixture));
    CHECK_STATUS(missing, Status::Decode);
    if (!missing) CHECK(missing.error().message.find("password required") != std::string::npos);
    // (Each attempt costs a 2^19-round key derivation, so one wrong password per archive.)
    const auto bad = test::read_as<ir::Archive>("7z", fixture7z(fixture), R"({"password":"Secret"})");
    CHECK_STATUS(bad, Status::Decode);
    if (!bad) CHECK(bad.error().message.find("wrong password") != std::string::npos);
  }
}

TEST_CASE("archive/7z: archives from p7zip, libarchive (bsdtar) and Deflate64") {
  // p7zip: LZMA2, directories with their Unix modes.
  const ir::Archive p7zip = read_ok("p7zip.7z");
  check_hello_lorem(p7zip);
  CHECK(test::str(find(p7zip, "exec.sh")->bytes) == "#!/bin/sh\necho hello\n");
  CHECK_EQ(find(p7zip, "exec.sh")->unix_mode, 0755u);
  CHECK(find(p7zip, "dir/empty_dir")->is_directory);
  CHECK(test::str(find(p7zip, "dir/nested/deep/file.txt")->bytes) == "deep file\n");
  // libarchive's 7-Zip writer: its own header layout, the symlink as S_IFLNK.
  const ir::Archive bsd = read_ok("bsdtar.7z");
  check_hello_lorem(bsd);
  CHECK_EQ(find(bsd, "exec.sh")->unix_mode, 0755u);
  CHECK(find(bsd, "dir/empty_dir")->is_directory);
  REQUIRE(find(bsd, "link") != nullptr);
  CHECK(find(bsd, "link")->symlink_target == "hello.txt");
  if (compression::can_decode(Method::Deflate64)) {
    check_hello_lorem(read_ok("deflate64.7z"));
  } else {
    MESSAGE("skipping deflate64.7z: Deflate64 decoder not available yet");
    CHECK(!test::read_as<ir::Archive>("7z", fixture7z("deflate64.7z")));
  }
}

// ============================================================================= hand-built archives

TEST_CASE("archive/7z: hand-built headers: anti items, dummies, attributes, encoded Copy header") {
  std::vector<RawFile> files = {
      raw("a.txt", "alpha", 0x20u | 0x8000u | (0100600u << 16)),
      raw("b.txt", "bravo"),  // no attributes
      raw("folder", "", 0x10u),
      raw("deleted.txt", ""),
      raw("win\\path.txt", "charlie", 0x20u),
  };
  files[2].directory = true;
  files[3].anti = true;
  for (const bool encoded : {false, true}) {
    INFO("encoded header ", encoded);
    const auto back = test::read_as<ir::Archive>("7z", build(files, {.encode_header = encoded}));
    REQUIRE_OK(back);
    REQUIRE_EQ(back->entries.size(), 4u);  // the anti item is not an entry
    CHECK(test::str(find(*back, "a.txt")->bytes) == "alpha");
    CHECK_EQ(find(*back, "a.txt")->unix_mode, 0600u);
    CHECK_EQ(find(*back, "b.txt")->unix_mode, 0u);
    CHECK(find(*back, "folder")->is_directory);
    CHECK(test::str(find(*back, "win/path.txt")->bytes) == "charlie");
    for (const auto& e : back->entries) CHECK_EQ(e.mtime, tree::mtime);
  }
  // A substream CRC mismatch.
  const auto crc = test::read_as<ir::Archive>("7z", build(files, {.bad_crc = true}));
  CHECK_STATUS(crc, Status::Decode);
  if (!crc) CHECK(crc.error().message.find("CRC") != std::string::npos);
  // Unsafe names are dropped by the sink.
  const auto unsafe = test::read_as<ir::Archive>("7z", build({raw("../evil.txt", "x"), raw("ok.txt", "y")}));
  REQUIRE_OK(unsafe);
  for (const auto& e : unsafe->entries) CHECK(e.path.find("..") == std::string::npos);
}

TEST_CASE("archive/7z: coder graphs: Delta and BCJ x86 chained behind Copy") {
  // Folder: coder 0 = filter (output), coder 1 = Copy (reads the packed stream).
  Bytes plain;
  for (int i = 0; i < 3000; ++i) plain.push_back(static_cast<std::byte>((i * 7) & 0xFF));
  for (const compression::Filter filter : {compression::Filter{compression::FilterKind::Delta, 4},
                                          compression::Filter{compression::FilterKind::X86, 0}}) {
    INFO("filter ", static_cast<int>(filter.kind));
    Bytes filtered = plain;
    compression::FilterState state;
    REQUIRE_OK(compression::filter_encode(filter, std::span<std::byte>(filtered), state, true));
    RawOptions o;
    sz::Coder f;
    if (filter.kind == compression::FilterKind::Delta) {
      f.id = sz::id_delta;
      f.properties = {std::byte{3}};  // distance - 1
    } else {
      f.id = sz::id_bcj;
    }
    o.folder.coders = {f, sz::Coder{}};
    o.folder.binds = {sz::BindPair{0, 1}};
    o.folder.packed = {1};
    o.folder.unpack_sizes = {plain.size(), plain.size()};
    o.packed = filtered;
    RawFile file;
    file.name = "data.bin";
    file.data = plain;
    const auto back = test::read_as<ir::Archive>("7z", build({file}, o));
    REQUIRE_OK(back);
    REQUIRE_EQ(back->entries.size(), 1u);
    CHECK(back->entries[0].bytes == plain);
  }
  // Two Copy coders chained by a bind pair, and an unknown method.
  RawOptions chained;
  chained.folder.coders = {sz::Coder{}, sz::Coder{}};
  chained.folder.binds = {sz::BindPair{0, 1}};
  chained.folder.packed = {1};
  chained.folder.unpack_sizes = {5, 5};
  REQUIRE_OK(test::read_as<ir::Archive>("7z", build({raw("x", "hello")}, chained)));
  RawOptions unknown;
  sz::Coder odd;
  odd.id = 0x7F7F7F;
  unknown.folder.coders = {odd};
  unknown.folder.packed = {0};
  const auto bad = test::read_as<ir::Archive>("7z", build({raw("x", "hello")}, unknown));
  CHECK_STATUS(bad, Status::Decode);
  if (!bad) CHECK(bad.error().message.find("not supported") != std::string::npos);
}

// ============================================================================= bombs and limits

TEST_CASE("archive/7z: declared sizes, limits and bombs") {
  // A Copy folder that claims a terabyte: refused before anything is allocated.
  const auto huge = test::read_as<ir::Archive>("7z", build({raw("x", "hello")}, {.unpack_size = 1ull << 40}));
  CHECK_STATUS(huge, Status::Limit);
  // 1 GiB is within the default 2 GiB budget, but far beyond the ratio guard for a
  // 300-byte archive: also refused before decoding.
  const auto ratio = test::read_as<ir::Archive>("7z", build({raw("x", "hello")}, {.unpack_size = 1ull << 30}));
  CHECK_STATUS(ratio, Status::Limit);
  // 8 MiB of zeros compress to a few hundred bytes; refused under a 1 MiB limit.
  ir::Archive zeros;
  ir::ArchiveEntry big;
  big.path = "zeros.bin";
  big.bytes = Bytes(8u << 20, std::byte{0});
  zeros.entries.push_back(big);
  const auto packed = test::write_from("7z", zeros);
  REQUIRE_OK(packed);
  CHECK_LT(packed->size(), 64u * 1024);
  Limits small;
  small.max_decompressed_bytes = 1u << 20;
  CHECK_STATUS(test::read_as<ir::Archive>("7z", *packed, "", nullptr, small), Status::Limit);
  REQUIRE_OK(test::read_as<ir::Archive>("7z", *packed));
  // The entry-count limit.
  Limits few;
  few.max_archive_entries = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("7z", fixture7z("tree_lzma2.7z"), "", nullptr, few), Status::Limit);
  // A header that claims 2^40 files.
  util::ByteWriter h;
  h.u8(sz::kHeader).u8(sz::kFilesInfo);
  sz::write_number(h, 1ull << 40);
  h.u8(sz::kEnd).u8(sz::kEnd);
  util::ByteWriter start;
  start.u64le(0).u64le(h.size()).u32le(util::crc32(h.view()));
  util::ByteWriter many;
  many.bytes(std::span<const std::uint8_t>(sz::signature.data(), sz::signature.size())).u8(0).u8(4);
  many.u32le(util::crc32(start.view())).bytes(start.view()).bytes(h.view());
  CHECK_STATUS(test::read_as<ir::Archive>("7z", many.view()), Status::Limit);
}

TEST_CASE("archive/7z: damaged archives fail cleanly") {
  for (const char* fixture : {"tree_lzma2.7z", "tree_lzma.7z", "copy.7z", "bcj2.7z", "arm64.7z", "bzip2.7z",
                              "deflate.7z", "encrypted_data.7z"}) {
    INFO("fixture ", fixture);
    test::fuzz_reader<ir::Archive>("7z", fixture7z(fixture));
  }
  // Every truncation of a small archive.
  const Bytes small = fixture7z("copy.7z");
  for (std::size_t n = 0; n < small.size(); n += 3) {
    CHECK(!test::read_as<ir::Archive>("7z", ByteView(small).first(n)));
  }
  CHECK_STATUS(test::read_as<ir::Archive>("7z", test::random_bytes(4096, 7)), Status::Decode);
  // A damaged start header and a damaged header CRC.
  Bytes start = small;
  start[20] ^= std::byte{1};
  CHECK_STATUS(test::read_as<ir::Archive>("7z", start), Status::Decode);
  Bytes tail = small;
  tail[tail.size() - 3] ^= std::byte{1};
  CHECK_STATUS(test::read_as<ir::Archive>("7z", tail), Status::Decode);
  // Version 1 is refused.
  Bytes version = small;
  version[6] = std::byte{1};
  CHECK_STATUS(test::read_as<ir::Archive>("7z", version), Status::Decode);
}

TEST_CASE("archive/7z: damaged encrypted archives never yield wrong content") {
  // Flips in the encrypted data and in the encrypted header, read with the right password:
  // either an error or (for flips in bytes nothing depends on, such as the
  // cipher padding) exactly the original content. Few positions: each read
  // derives the key again.
  const Bytes good = fixture7z("encrypted.7z");
  for (const std::size_t at : {std::size_t{40}, std::size_t{200}, good.size() - 120, good.size() - 40}) {
    INFO("flipped byte ", at);
    Bytes bad = good;
    bad[at] ^= std::byte{0x01};
    const auto back = test::read_as<ir::Archive>("7z", bad, R"({"password":"secret"})");
    if (back) {
      check_hello_lorem(*back);
    } else {
      CHECK_EQ(back.error().code, Status::Decode);
    }
  }
}

// ============================================================================= writer

namespace {

std::string options_json(std::string_view method, bool solid, std::string_view password = {},
                         std::optional<bool> encrypt_headers = std::nullopt) {
  std::string o = std::string(R"({"method":")") + std::string(method) + "\",\"solid\":" + (solid ? "true" : "false");
  if (!password.empty()) o += std::string(R"(,"password":")") + std::string(password) + "\"";
  if (encrypt_headers) o += std::string(R"(,"encrypt_headers":)") + (*encrypt_headers ? "true" : "false");
  return o + "}";
}

}  // namespace

TEST_CASE("archive/7z: writer round trips for every method, solid and not") {
  const ir::Archive source = tree::archive();
  for (const char* method : {"lzma2", "lzma", "bzip2", "deflate", "copy", "ppmd"}) {
    for (const bool solid : {true, false}) {
      INFO("method ", method, " solid ", solid);
      const std::string options = options_json(method, solid);
      const auto written = test::write_from("7z", source, options);
      REQUIRE_OK(written);
      const auto back = test::read_as<ir::Archive>("7z", *written);
      REQUIRE_OK(back);
      tree::check(*back);
      CHECK(find(*back, "dir/empty_dir")->is_directory);
      CHECK(*test::write_from("7z", source, options) == *written);
    }
  }
}

namespace {

// One encrypted round trip: right, wrong and missing password, determinism, and
// whether the kEncodedHeader record at the end names 7zAES (encrypted header).
// Every write and every read with a password derives the key (2^19 SHA-256
// rounds), so each case does only what it needs.
void encrypted_round_trip(const std::string& options, bool headers_encrypted, bool check_determinism) {
  const ir::Archive source = tree::archive();
  const auto written = test::write_from("7z", source, options);
  REQUIRE_OK(written);
  const auto back = test::read_as<ir::Archive>("7z", *written, R"({"password":"pässwörd"})");
  REQUIRE_OK(back);
  tree::check(*back);
  const auto missing = test::read_as<ir::Archive>("7z", *written);
  CHECK_STATUS(missing, Status::Decode);
  if (!missing) CHECK(missing.error().message.find("password required") != std::string::npos);
  const auto wrong = test::read_as<ir::Archive>("7z", *written, R"({"password":"passwort"})");
  CHECK_STATUS(wrong, Status::Decode);
  if (!wrong) CHECK(wrong.error().message.find("wrong password") != std::string::npos);
  if (check_determinism) CHECK(*test::write_from("7z", source, options) == *written);
  const ByteView end = ByteView(*written).last(std::min<std::size_t>(written->size(), 80));
  CHECK_EQ(contains(end, test::bytes("\x06\xF1\x07\x01")), headers_encrypted);
  CHECK(!contains(*written, as_bytes(std::string_view("Lorem ipsum"))));
}

}  // namespace

TEST_CASE("archive/7z: writer encryption, solid, headers encrypted by default") {
  encrypted_round_trip(R"({"password":"pässwörd"})", true, true);
}

TEST_CASE("archive/7z: writer encryption, not solid, plain headers") {
  encrypted_round_trip(options_json("lzma2", false, "pässwörd", false), false, false);
}

TEST_CASE("archive/7z: writer encryption with Copy (AES behind the Copy coder)") {
  const ir::Archive source = tree::archive();
  for (const char* method : {"copy"}) {
    INFO("method ", method);
    const auto written = test::write_from("7z", source, options_json(method, true, "pässwörd", true));
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Archive>("7z", *written, R"({"password":"pässwörd"})");
    REQUIRE_OK(back);
    tree::check(*back);
  }
}

TEST_CASE("archive/7z: writer policy, levels and layout") {
  const ir::Archive source = tree::archive();
  const auto written = test::write_from("7z", source);
  REQUIRE_OK(written);
  CHECK(test::str(*written).starts_with(std::string_view("7z\xBC\xAF\x27\x1C\x00\x04", 8)));
  // Level 0 stores everything (Copy); higher levels are not larger on text.
  const auto stored = test::write_from("7z", source, R"({"compression_level":0})");
  REQUIRE_OK(stored);
  CHECK_GT(stored->size(), written->size());
  tree::check(*test::read_as<ir::Archive>("7z", *stored));
  ir::Archive text;
  ir::ArchiveEntry big;
  big.path = "big.txt";
  for (int i = 0; i < 300; ++i) append(big.bytes, as_bytes(tree::lorem_paragraph() + std::to_string(i * 7919) + "\n"));
  text.entries.push_back(big);
  std::size_t previous = 0;
  for (const int level : {1, 5, 9}) {
    const auto packed = test::write_from("7z", text, std::format(R"({{"compression_level":{}}})", level));
    REQUIRE_OK(packed);
    if (previous != 0) CHECK_LE(packed->size(), previous + 16);
    previous = packed->size();
  }
  // Incompressible data falls back to Copy: barely larger than the input.
  ir::Archive noise;
  ir::ArchiveEntry n;
  n.path = "noise.bin";
  n.bytes = test::random_bytes(10000, 3);
  noise.entries.push_back(n);
  const auto noisy = test::write_from("7z", noise, R"({"method":"bzip2"})");
  REQUIRE_OK(noisy);
  CHECK_LT(noisy->size(), 10000u + 200u);
  CHECK(test::read_as<ir::Archive>("7z", *noisy)->entries[0].bytes == n.bytes);
  // Fixed timestamps.
  const auto fixed = test::write_from("7z", source, R"({"timestamps":"fixed"})");
  REQUIRE_OK(fixed);
  const auto fixed_back = test::read_as<ir::Archive>("7z", *fixed);  // named: the loop must not outlive it
  REQUIRE_OK(fixed_back);
  for (const auto& e : fixed_back->entries) CHECK_EQ(e.mtime, 315532800);
  // strip_metadata: permissions normalised (exec.sh keeps 0755, files 0644).
  ir::Archive odd;
  ir::ArchiveEntry f;
  f.path = "private.txt";
  f.bytes = test::bytes("x");
  f.unix_mode = 0600;
  odd.entries.push_back(f);
  const auto normal = test::read_as<ir::Archive>("7z", *test::write_from("7z", odd));
  REQUIRE_OK(normal);
  const auto kept = test::read_as<ir::Archive>("7z", *test::write_from("7z", odd, R"({"strip_metadata":false})"));
  REQUIRE_OK(kept);
  CHECK_EQ(kept->entries[0].unix_mode, 0600u);
  // An empty archive is a bare signature header that reads back empty.
  const auto empty = test::write_from("7z", ir::Archive{});
  REQUIRE_OK(empty);
  CHECK_EQ(empty->size(), 32u);
  const auto empty_back = test::read_as<ir::Archive>("7z", *empty);
  REQUIRE_OK(empty_back);
  CHECK(empty_back->entries.empty());
  // Only directories: no streams at all.
  ir::Archive dirs;
  ir::ArchiveEntry d;
  d.path = "only/dir";
  d.is_directory = true;
  dirs.entries.push_back(d);
  const auto dir_back = test::read_as<ir::Archive>("7z", *test::write_from("7z", dirs));
  REQUIRE_OK(dir_back);
  REQUIRE_EQ(dir_back->entries.size(), 1u);
  CHECK(dir_back->entries[0].is_directory);
}

TEST_CASE("archive/7z: writer output of about 1 MiB") {
  ir::Archive source;
  ir::ArchiveEntry a;
  a.path = "text.txt";
  for (int i = 0; i < 2500; ++i) append(a.bytes, as_bytes(tree::lorem_paragraph()));
  ir::ArchiveEntry b;
  b.path = "noise.bin";
  b.bytes = test::random_bytes(200000, 11);
  source.entries = {a, b};
  const auto written = test::write_from("7z", source);
  REQUIRE_OK(written);
  CHECK_LT(written->size(), 220000u);
  const auto back = test::read_as<ir::Archive>("7z", *written);
  REQUIRE_OK(back);
  CHECK(find(*back, "text.txt")->bytes == a.bytes);
  CHECK(find(*back, "noise.bin")->bytes == b.bytes);
  test::fuzz_reader<ir::Archive>("7z", *test::write_from("7z", tree::archive()), 128);
}

// ============================================================================= conversions and detection

TEST_CASE("archive/7z: conversions through the engine and the compress route") {
  // tar -> 7z keeps everything, including the symlink and the empty directory.
  const test::Converted to_7z = test::convert(test::fixture("archive/tar/tree_pax.tar"), "", "7z");
  REQUIRE(to_7z.ok());
  CHECK(to_7z.format_id == "7z");
  tree::check(*test::read_as<ir::Archive>("7z", to_7z.data));
  // 7z -> tar.
  const test::Converted to_tar = test::convert(fixture7z("tree_lzma2.7z"), "", "tar");
  REQUIRE(to_tar.ok());
  tree::check(*test::read_as<ir::Archive>("tar", to_tar.data));
  // 7z -> 7z recompresses (level 9, LZMA2, solid): the Copy archive shrinks.
  const Bytes copy = fixture7z("copy.7z");
  const test::Converted again = test::convert(copy, "", "7z");
  REQUIRE(again.ok());
  CHECK(again.format_id == "7z");
  CHECK_LT(again.data.size(), copy.size() / 2);
  check_hello_lorem(*test::read_as<ir::Archive>("7z", again.data));
  // 7z -> zip.
  const test::Converted to_zip = test::convert(fixture7z("tree_lzma2.7z"), "", "zip");
  REQUIRE(to_zip.ok());
  tree::check(*test::read_as<ir::Archive>("zip", to_zip.data));
}

TEST_CASE("archive/7z: an encrypted archive converts with its password") {
  // Read with the password and re-encrypted with it.
  const test::Converted reenc = test::convert(fixture7z("encrypted.7z"), "", "7z", R"({"password":"secret"})");
  REQUIRE(reenc.ok());
  check_hello_lorem(*test::read_as<ir::Archive>("7z", reenc.data, R"({"password":"secret"})"));
  CHECK(!test::read_as<ir::Archive>("7z", reenc.data));
}

TEST_CASE("archive/7z: detection") {
  const auto detected = [](ByteView data, std::string_view name = {}) {
    const auto d = detect_format(Registry::instance(), data, name);
    return d ? d->format->id : std::string("none");
  };
  for (const char* fixture : {"tree_lzma2.7z", "copy.7z", "encrypted.7z"}) {
    CHECK(detected(fixture7z(fixture)) == "7z");
    CHECK(detected(fixture7z(fixture), "archive.7z") == "7z");
  }
  CHECK(detected(*test::write_from("7z", ir::Archive{})) == "7z");
  CHECK(detected(test::bytes("7z is a nice format"), "notes.txt") != "7z");
  const auto info = Registry::instance().find("7z");
  REQUIRE(info != nullptr);
  CHECK(info->mime == "application/x-7z-compressed");
  CHECK(Registry::instance().validate().empty());
}
