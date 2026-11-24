// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// tar in every flavour, the compressed tar.* formats and single-file streams.
#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "families/archive/compression.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;

namespace {
ir::Archive read_ok(std::string_view format, std::string_view fixture) {
  INFO("fixture ", fixture);
  auto archive = test::read_as<ir::Archive>(format, test::fixture(std::string("archive/") + std::string(fixture)));
  REQUIRE_OK(archive);
  return std::move(*archive);
}
Bytes gunzip(std::string_view fixture) {
  auto out = compression::decompress(compression::Method::Gzip, test::fixture(std::string("archive/") + std::string(fixture)));
  REQUIRE_OK(out);
  return std::move(*out);
}
}  // namespace

TEST_CASE("archive/tar: GNU tar and bsdtar archives extract exactly") {
  tree::check(read_ok("tar", "tar/tree_gnu.tar"));
  tree::check(read_ok("tar", "tar/tree_pax.tar"));
  tree::check(read_ok("tar", "tar/tree_bsd_pax.tar"));
  // ustar: subset without the long and unicode names
  tree::check(read_ok("tar", "tar/tree_ustar.tar"), {.empty_dir = true, .unicode = false, .long_name = false});
  // v7: no directories of its own, no unicode/long names
  tree::check(read_ok("tar", "tar/tree_v7.tar"), {.empty_dir = false, .unicode = false, .long_name = false, .noise = false});
}

TEST_CASE("archive/tar: hard links and sparse files") {
  const ir::Archive links = read_ok("tar", "tar/hardlink.tar");
  REQUIRE_EQ(links.entries.size(), 2u);
  CHECK(test::str(links.entries[1].bytes) == "Hello, archive!\n");
  for (const char* name : {"tar/sparse_gnu.tar.gz", "tar/sparse_pax10.tar.gz", "tar/sparse_pax01.tar.gz"}) {
    INFO("fixture ", name);
    const auto archive = test::read_as<ir::Archive>("tar", gunzip(name));
    REQUIRE_OK(archive);
    REQUIRE_EQ(archive->entries.size(), 1u);
    const Bytes& data = archive->entries[0].bytes;
    REQUIRE_EQ(data.size(), 200003u);
    CHECK(test::str(ByteView(data).first(5)) == "begin");
    CHECK(test::str(ByteView(data).last(3)) == "end");
    CHECK(std::all_of(data.begin() + 5, data.end() - 3, [](std::byte b) { return b == std::byte{0}; }));
  }
}

TEST_CASE("archive/tar: compressed tar formats from the reference tools") {
  struct Case {
    const char* format;
    const char* fixture;
    compression::Method method;
  };
  for (const Case& c : {Case{"tar.gz", "tar/tree.tar.gz", compression::Method::Gzip},
                        Case{"tar.bz2", "tar/tree.tar.bz2", compression::Method::Bzip2},
                        Case{"tar.xz", "tar/tree.tar.xz", compression::Method::Xz},
                        Case{"tar.zst", "tar/tree.tar.zst", compression::Method::Zstd},
                        Case{"tar.lz4", "tar/tree.tar.lz4", compression::Method::Lz4Frame},
                        Case{"tar.lz", "tar/tree.tar.lz", compression::Method::Lzip},
                        Case{"tar.lzma", "tar/tree.tar.lzma", compression::Method::LzmaAlone},
                        Case{"tar.z", "tar/tree.tar.Z", compression::Method::Lzw},
                        Case{"tar.br", "tar/tree.tar.br", compression::Method::Brotli}}) {
    INFO("format ", c.format);
    if (!compression::can_decode(c.method)) {
      MESSAGE("skipping ", c.format, ": codec not available in this build");
      continue;
    }
    tree::check(read_ok(c.format, c.fixture));
    // Detection by content picks the compound format, not the bare stream.
    const test::Converted detected = test::convert(test::fixture(std::string("archive/") + c.fixture), "", "tar");
    CHECK(detected.ok());
  }
}

TEST_CASE("archive/tar: writer round trips in every flavour and through every compressor") {
  const ir::Archive source = tree::archive();
  for (const char* format : {"pax", "gnu"}) {
    INFO("tar_format ", format);
    const auto written = test::write_from("tar", source, std::string(R"({"tar_format":")") + format + R"("})");
    REQUIRE_OK(written);
    CHECK_EQ(written->size() % 10240, 0u);
    const auto back = test::read_as<ir::Archive>("tar", *written);
    REQUIRE_OK(back);
    tree::check(*back);
  }
  // ustar cannot hold the 124-byte name.
  CHECK_STATUS(test::write_from("tar", source, R"({"tar_format":"ustar"})"), Status::Encode);
  for (const char* format : {"tar.gz", "tar.bz2", "tar.xz", "tar.zst", "tar.lz4", "tar.lz", "tar.lzma", "tar.z", "tar.sz"}) {
    INFO("format ", format);
    const auto written = test::write_from(format, source);
    if (!written && written.error().code == Status::UnsupportedConversion) continue;
    REQUIRE_OK(written);
    const auto back = test::read_as<ir::Archive>(format, *written);
    REQUIRE_OK(back);
    tree::check(*back);
  }
}

TEST_CASE("archive/tar: privacy and policy of the writer") {
  ir::Archive source = tree::archive();
  source.entries[6].unix_mode = 04711;  // exec.sh with setuid
  const auto written = test::write_from("tar", source);
  REQUIRE_OK(written);
  // No owner names, owner 0, normalised modes by default.
  const auto back = test::read_as<ir::Archive>("tar", *written);
  REQUIRE_OK(back);
  for (const auto& e : back->entries) {
    if (e.path == "exec.sh") CHECK_EQ(e.unix_mode, 0755u);
  }
  CHECK(test::str(*written).find("root") == std::string::npos);
  // timestamps = fixed writes 1980-01-01 everywhere.
  const auto fixed = test::write_from("tar", source, R"({"timestamps":"fixed"})");
  REQUIRE_OK(fixed);
  const auto fixed_back = test::read_as<ir::Archive>("tar", *fixed);
  REQUIRE_OK(fixed_back);
  for (const auto& e : fixed_back->entries) CHECK_EQ(e.mtime, 315532800);
  // Deterministic.
  CHECK(*test::write_from("tar", source) == *written);
}

TEST_CASE("archive/streams: single compressed files keep or restore their names") {
  const auto named = test::read_as<ir::Archive>("gz", test::fixture("archive/streams/lorem_named.gz"));
  REQUIRE_OK(named);
  REQUIRE_EQ(named->entries.size(), 1u);
  CHECK(named->entries[0].path == "lorem.txt");
  CHECK_EQ(named->entries[0].mtime, tree::mtime);
  // Without a stored name the file name minus ".gz" is used.
  const test::Converted out = test::convert(test::fixture("archive/streams/hello_noname.gz"), "", "tar", "", "greeting.txt.gz");
  REQUIRE(out.ok());
  const auto as_tar = test::read_as<ir::Archive>("tar", out.data);
  REQUIRE_OK(as_tar);
  REQUIRE_EQ(as_tar->entries.size(), 1u);
  CHECK(as_tar->entries[0].path == "greeting.txt");
  CHECK(test::str(as_tar->entries[0].bytes) == "Hello, archive!\n");
  // Any file can be compressed: Blob -> Archive -> gz, and back.
  for (const char* format : {"gz", "bz2", "xz", "lzma", "zst", "lz4", "lz", "z", "zlib", "sz"}) {
    INFO("format ", format);
    const test::Converted packed = test::convert(as_bytes("some text to compress, some text to compress"), "txt", format, "", "note.txt");
    REQUIRE(packed.ok());
    const auto back = test::read_as<ir::Archive>(format, packed.data);
    REQUIRE_OK(back);
    REQUIRE_EQ(back->entries.size(), 1u);
    CHECK(test::str(back->entries[0].bytes) == "some text to compress, some text to compress");
  }
  // gzip writes FNAME, so the name comes back.
  const test::Converted gz = test::convert(as_bytes("abc"), "txt", "gz", "", "note.txt");
  REQUIRE(gz.ok());
  CHECK(test::read_as<ir::Archive>("gz", gz.data)->entries[0].path == "note.txt");
  // More than one file does not fit.
  CHECK_STATUS(test::write_from("gz", tree::archive()), Status::Encode);
}

TEST_CASE("archive/tar: damaged archives fail cleanly") {
  test::fuzz_reader<ir::Archive>("tar", test::fixture("archive/tar/tree_gnu.tar"));
  test::fuzz_reader<ir::Archive>("tar.gz", test::fixture("archive/tar/tree.tar.gz"));
  test::fuzz_reader<ir::Archive>("tar.xz", test::fixture("archive/tar/tree.tar.xz"));
  test::fuzz_reader<ir::Archive>("gz", test::fixture("archive/streams/lorem_named.gz"));
  CHECK_STATUS(test::read_as<ir::Archive>("tar", test::random_bytes(2048, 5)), Status::Decode);
  // Entry-count limit.
  Limits few;
  few.max_archive_entries = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("tar", test::fixture("archive/tar/tree_gnu.tar"), "", nullptr, few), Status::Limit);
  // Decompression bomb: a gzip of zeros declared as tar.gz is stopped by the limit.
  const auto zeros = compression::compress(compression::Method::Gzip, Bytes(20u << 20, std::byte{0}));
  Limits small;
  small.max_decompressed_bytes = 1u << 20;
  CHECK_STATUS(test::read_as<ir::Archive>("tar.gz", *zeros, "", nullptr, small), Status::Limit);
}

TEST_CASE("archive/tar: detection and the compress route") {
  CHECK(test::convert(test::fixture("archive/tar/tree_pax.tar"), "", "tar").ok());
  const test::Converted again = test::convert(test::fixture("archive/tar/tree.tar.gz"), "", "tar.gz");
  REQUIRE(again.ok());
  CHECK(again.format_id == "tar.gz");
  const auto back = test::read_as<ir::Archive>("tar.gz", again.data);
  REQUIRE_OK(back);
  tree::check(*back);
  // tar -> tar.xz -> zip-free round trip through the engine.
  const test::Converted xz = test::convert(test::fixture("archive/tar/tree_gnu.tar"), "tar", "tar.xz");
  REQUIRE(xz.ok());
  tree::check(*test::read_as<ir::Archive>("tar.xz", xz.data));
}
