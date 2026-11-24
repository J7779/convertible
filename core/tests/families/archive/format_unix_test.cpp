// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The Unix archive formats: cpio, ar, deb, rpm, shar, warc and xar. Real
// archives from GNU cpio, binutils ar, bsdtar, dpkg-deb and rpmbuild, the
// writers (cpio, ar, shar), conversions, limits and damaged input.
#include <format>
#include <map>

#include "families/archive/tree_fixture.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

#include "engine/detect.hpp"
#include "families/archive/compression.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

using namespace cvt;
namespace tree = cvt::test::tree;
using compression::Method;

namespace {

Bytes unix_fixture(std::string_view name) { return test::fixture("archive/unix/" + std::string(name)); }

ir::Archive read_ok(std::string_view format, std::string_view fixture, std::vector<std::string>* warnings = nullptr) {
  INFO("fixture ", fixture);
  auto archive = test::read_as<ir::Archive>(format, unix_fixture(fixture), "", warnings);
  REQUIRE_OK(archive);
  return std::move(*archive);
}

std::map<std::string, const ir::ArchiveEntry*> by_path(const ir::Archive& archive) {
  std::map<std::string, const ir::ArchiveEntry*> out;
  for (const ir::ArchiveEntry& e : archive.entries) out[e.path] = &e;
  return out;
}

const Bytes& lorem() {
  static const Bytes bytes = tree::files()[1].bytes;
  return bytes;
}

std::string detected(ByteView data, std::string_view name = {}) {
  const auto d = detect_format(Registry::instance(), data, name);
  return d ? d->format->id : std::string("none");
}

// The little-endian old binary cpio archive rewritten in big-endian order
// (every 16-bit header word swapped; names and data unchanged).
Bytes to_big_endian_cpio(Bytes data) {
  std::size_t pos = 0;
  while (pos + 26 <= data.size()) {
    const auto word = [&](std::size_t i) {
      return std::to_integer<std::size_t>(data[pos + 2 * i]) | (std::to_integer<std::size_t>(data[pos + 2 * i + 1]) << 8);
    };
    const std::size_t namesize = word(10);
    const std::size_t filesize = (word(11) << 16) | word(12);
    const bool trailer = std::string_view(reinterpret_cast<const char*>(data.data()) + pos + 26, 10) == "TRAILER!!!";
    for (std::size_t i = 0; i < 13; ++i) std::swap(data[pos + 2 * i], data[pos + 2 * i + 1]);
    pos += 26 + namesize + (namesize & 1u);
    pos += filesize + (filesize & 1u);
    if (trailer) break;
  }
  return data;
}

}  // namespace

// ------------------------------------------------------------------------------- cpio

TEST_CASE("archive/cpio: GNU cpio newc, crc and odc archives extract exactly") {
  for (const char* name : {"tree_newc.cpio", "tree_crc.cpio", "tree_odc.cpio"}) {
    std::vector<std::string> warnings;
    const ir::Archive archive = read_ok("cpio", name, &warnings);
    tree::check(archive);
    // The "." entry of `find .` is dropped silently.
    CHECK(warnings.empty());
  }
}

TEST_CASE("archive/cpio: old binary archives in both byte orders") {
  const tree::Check subset{.symlink = false, .unicode = false, .long_name = false, .noise = false};
  const Bytes le = unix_fixture("tree_bin.cpio");
  const auto little = test::read_as<ir::Archive>("cpio", le);
  REQUIRE_OK(little);
  tree::check(*little, subset);
  const Bytes be = to_big_endian_cpio(le);
  CHECK(be != le);
  const auto big = test::read_as<ir::Archive>("cpio", be);
  REQUIRE_OK(big);
  tree::check(*big, subset);
  CHECK(detected(be) == "cpio");
}

TEST_CASE("archive/cpio: crc checksums are verified") {
  Bytes damaged = unix_fixture("tree_crc.cpio");
  // Flip a byte of hello.txt's content.
  const std::string_view text(reinterpret_cast<const char*>(damaged.data()), damaged.size());
  const std::size_t at = text.find("Hello, archive!");
  REQUIRE(at != std::string_view::npos);
  damaged[at] = std::byte{'J'};
  CHECK_STATUS(test::read_as<ir::Archive>("cpio", damaged), Status::Decode);
  // The same change in newc (no checksum) is read as it is.
  Bytes newc = unix_fixture("tree_newc.cpio");
  const std::size_t at2 = std::string_view(reinterpret_cast<const char*>(newc.data()), newc.size()).find("Hello, archive!");
  newc[at2] = std::byte{'J'};
  CHECK_OK(test::read_as<ir::Archive>("cpio", newc));
}

TEST_CASE("archive/cpio: hard links share their content") {
  // newc stores the data once, with the last link (as GNU cpio does).
  const auto entry = [](std::string_view name, std::uint32_t ino, std::uint32_t nlink, std::string_view data) {
    std::string h = std::format("070701{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}{:08X}",
                                ino, 0100644u, 0u, 0u, nlink, 1623758400u, data.size(), 0u, 1u, 0u, 0u, name.size() + 1, 0u);
    h += name;
    h += '\0';
    while (h.size() % 4 != 0) h += '\0';
    h += data;
    while (h.size() % 4 != 0) h += '\0';
    return h;
  };
  const std::string cpio = entry("a.txt", 7, 2, "") + entry("b.txt", 7, 2, "shared\n") + entry("c.txt", 8, 1, "own\n") +
                           entry("TRAILER!!!", 0, 1, "");
  const auto archive = test::read_as<ir::Archive>("cpio", as_bytes(cpio));
  REQUIRE_OK(archive);
  REQUIRE_EQ(archive->entries.size(), 3u);
  CHECK(test::str(archive->entries[0].bytes) == "shared\n");
  CHECK(test::str(archive->entries[1].bytes) == "shared\n");
  CHECK(test::str(archive->entries[2].bytes) == "own\n");
}

TEST_CASE("archive/cpio: writer round trip, determinism and privacy") {
  const ir::Archive source = tree::archive();
  const auto written = test::write_from("cpio", source);
  REQUIRE_OK(written);
  CHECK(test::str(ByteView(*written).first(6)) == "070701");
  CHECK_EQ(written->size() % 512, 0u);
  const auto back = test::read_as<ir::Archive>("cpio", *written);
  REQUIRE_OK(back);
  tree::check(*back);
  CHECK(*test::write_from("cpio", source) == *written);
  // Owner 0 everywhere: uid and gid fields (bytes 22..37 of every header) are zero.
  const std::string text = test::str(*written);
  for (std::size_t at = text.find("070701"); at != std::string::npos; at = text.find("070701", at + 110)) {
    CHECK(text.substr(at + 22, 16) == std::string(16, '0'));
  }
  // timestamps = fixed and strip_metadata.
  ir::Archive odd = source;
  for (auto& e : odd.entries) {
    if (e.path == "exec.sh") e.unix_mode = 04711;
  }
  const auto fixed = test::write_from("cpio", odd, R"({"timestamps":"fixed"})");
  REQUIRE_OK(fixed);
  const auto fixed_back = test::read_as<ir::Archive>("cpio", *fixed);
  REQUIRE_OK(fixed_back);
  for (const auto& e : fixed_back->entries) {
    CHECK_EQ(e.mtime, 315532800);
    if (e.path == "exec.sh") CHECK_EQ(e.unix_mode, 0755u);
  }
  // Empty archive: only the trailer.
  const auto empty = test::write_from("cpio", ir::Archive{});
  REQUIRE_OK(empty);
  const auto empty_back = test::read_as<ir::Archive>("cpio", *empty);
  REQUIRE_OK(empty_back);
  CHECK(empty_back->entries.empty());
  // A large-ish incompressible file.
  ir::Archive big;
  big.entries.push_back({"big.bin", test::random_bytes(1 << 20, 3), false, tree::mtime, 0644, {}});
  const auto big_written = test::write_from("cpio", big);
  REQUIRE_OK(big_written);
  const auto big_back = test::read_as<ir::Archive>("cpio", *big_written);
  REQUIRE_OK(big_back);
  REQUIRE_EQ(big_back->entries.size(), 1u);
  CHECK(big_back->entries[0].bytes == big.entries[0].bytes);
}

// ------------------------------------------------------------------------------- ar

TEST_CASE("archive/ar: GNU ar archive with a long-name table") {
  const ir::Archive archive = read_ok("ar", "tree.a");
  const auto files = by_path(archive);
  REQUIRE_EQ(archive.entries.size(), 5u);
  CHECK(test::str(files.at("hello.txt")->bytes) == "Hello, archive!\n");
  CHECK(files.at("lorem.txt")->bytes == lorem());
  CHECK(files.at("noise.bin")->bytes == tree::noise(4096));
  CHECK(test::str(files.at("exec.sh")->bytes) == "#!/bin/sh\necho hello\n");
  CHECK(test::str(files.at("long_member_name_for_gnu_ar.txt")->bytes) == "long name\n");
  // `ar rcD`: deterministic mode, so no times and owner 0.
  CHECK_EQ(files.at("hello.txt")->mtime, 0);
  CHECK_EQ(files.at("hello.txt")->unix_mode, 0644u);
}

TEST_CASE("archive/ar: BSD ar archives, short and '#1/' long names") {
  const ir::Archive archive = read_ok("ar", "tree_bsd.a");
  const auto files = by_path(archive);
  REQUIRE_EQ(archive.entries.size(), 3u);
  CHECK(files.at("lorem.txt")->bytes == lorem());
  CHECK_EQ(files.at("exec.sh")->unix_mode, 0755u);
  CHECK_EQ(files.at("exec.sh")->mtime, tree::mtime);
  const ir::Archive longer = read_ok("ar", "tree_bsd_long.a");
  REQUIRE_EQ(longer.entries.size(), 2u);
  CHECK(longer.entries[1].path == "long_member_name_for_bsd_ar.txt");
  CHECK(test::str(longer.entries[1].bytes) == "long name\n");
  CHECK(test::str(longer.entries[0].bytes) == "Hello, archive!\n");
}

TEST_CASE("archive/ar: thin archives are refused") {
  const std::string thin = "!<thin>\n/               0           0     0     0       0         `\n";
  CHECK_STATUS(test::read_as<ir::Archive>("ar", as_bytes(thin)), Status::Decode);
}

TEST_CASE("archive/ar: writer round trip with long names, flattening and determinism") {
  ir::Archive source;
  source.entries.push_back({"hello.txt", test::bytes("Hello, archive!\n"), false, tree::mtime, 0644, {}});
  source.entries.push_back({"odd.bin", test::bytes("odd"), false, tree::mtime, 0755, {}});
  source.entries.push_back({"a_rather_long_member_name.txt", test::bytes("long\n"), false, tree::mtime, 0644, {}});
  source.entries.push_back({"with space.txt", test::bytes("space\n"), false, tree::mtime, 0644, {}});
  source.entries.push_back({"sub/dir/nested.txt", test::bytes("nested\n"), false, tree::mtime, 0644, {}});
  source.entries.push_back({"sub", {}, true, tree::mtime, 0755, {}});
  source.entries.push_back({"link", {}, false, tree::mtime, 0777, "hello.txt"});
  std::vector<std::string> warnings;
  const auto written = test::write_from("ar", source, "", &warnings);
  REQUIRE_OK(written);
  CHECK(test::str(ByteView(*written).first(8)) == "!<arch>\n");
  CHECK(test::str(*written).find("//") == 8);
  CHECK_EQ(warnings.size(), 2u);  // symlink dropped, directories flattened
  const auto back = test::read_as<ir::Archive>("ar", *written);
  REQUIRE_OK(back);
  REQUIRE_EQ(back->entries.size(), 5u);
  const auto files = by_path(*back);
  CHECK(test::str(files.at("odd.bin")->bytes) == "odd");
  CHECK_EQ(files.at("odd.bin")->unix_mode, 0755u);
  CHECK_EQ(files.at("odd.bin")->mtime, tree::mtime);
  CHECK(test::str(files.at("a_rather_long_member_name.txt")->bytes) == "long\n");
  CHECK(test::str(files.at("with space.txt")->bytes) == "space\n");
  CHECK(test::str(files.at("nested.txt")->bytes) == "nested\n");
  CHECK(*test::write_from("ar", source) == *written);
  // The tree: every file comes back under its base name.
  const auto tree_written = test::write_from("ar", tree::archive());
  REQUIRE_OK(tree_written);
  const auto tree_back = test::read_as<ir::Archive>("ar", *tree_written);
  REQUIRE_OK(tree_back);
  CHECK_EQ(tree_back->entries.size(), tree::files().size());
  CHECK(by_path(*tree_back).at(tree::long_name())->bytes == test::bytes("long name\n"));
  // Empty archive.
  const auto empty = test::write_from("ar", ir::Archive{});
  REQUIRE_OK(empty);
  CHECK(test::str(*empty) == "!<arch>\n");
}

// ------------------------------------------------------------------------------- deb

TEST_CASE("archive/deb: dpkg-deb packages with xz, gzip and zstd members") {
  for (const auto& [name, method] : {std::pair{"demo_xz.deb", Method::Xz}, std::pair{"demo_gz.deb", Method::Gzip},
                                     std::pair{"demo_zst.deb", Method::Zstd}}) {
    INFO("fixture ", name);
    if (!compression::can_decode(method)) {
      MESSAGE("skipping ", name, ": codec not available in this build");
      continue;
    }
    const ir::Archive archive = read_ok("deb", name);
    const auto files = by_path(archive);
    REQUIRE(files.contains("DEBIAN/control"));
    CHECK(test::str(files.at("DEBIAN/control")->bytes) ==
          "Package: cvt-demo\nVersion: 1.0\nArchitecture: all\nMaintainer: Nobody <nobody@invalid>\n"
          "Description: synthetic test package\n");
    REQUIRE(files.contains("usr/share/doc/cvt-demo/hello.txt"));
    CHECK(test::str(files.at("usr/share/doc/cvt-demo/hello.txt")->bytes) == "Hello, archive!\n");
    CHECK(files.at("usr/share/doc/cvt-demo/lorem.txt")->bytes == lorem());
    CHECK_EQ(files.at("usr/share/doc/cvt-demo/lorem.txt")->mtime, tree::mtime);
    CHECK_EQ(files.at("usr/share/doc/cvt-demo/lorem.txt")->unix_mode, 0644u);
    CHECK(files.at("usr/share/doc/cvt-demo")->is_directory);
    CHECK(files.at("DEBIAN")->is_directory);
    CHECK(detected(unix_fixture(name)) == "deb");
  }
}

TEST_CASE("archive/deb: converts to tar.gz and zip") {
  if (!compression::can_decode(Method::Xz)) return;
  const test::Converted tgz = test::convert(unix_fixture("demo_xz.deb"), "", "tar.gz");
  REQUIRE(tgz.ok());
  const auto back = test::read_as<ir::Archive>("tar.gz", tgz.data);
  REQUIRE_OK(back);
  CHECK(by_path(*back).at("usr/share/doc/cvt-demo/lorem.txt")->bytes == lorem());
  const test::Converted zip = test::convert(unix_fixture("demo_xz.deb"), "deb", "zip");
  REQUIRE(zip.ok());
  CHECK(by_path(*test::read_as<ir::Archive>("zip", zip.data)).contains("DEBIAN/control"));
}

// ------------------------------------------------------------------------------- rpm

TEST_CASE("archive/rpm: rpmbuild packages with gzip, xz, zstd and bzip2 payloads") {
  for (const auto& [name, method] : {std::pair{"demo_gz.rpm", Method::Gzip}, std::pair{"demo_xz.rpm", Method::Xz},
                                     std::pair{"demo_zst.rpm", Method::Zstd}, std::pair{"demo_bz2.rpm", Method::Bzip2}}) {
    INFO("fixture ", name);
    if (!compression::can_decode(method)) {
      MESSAGE("skipping ", name, ": codec not available in this build");
      continue;
    }
    std::vector<std::string> warnings;
    const ir::Archive archive = read_ok("rpm", name, &warnings);
    CHECK(warnings.empty());
    REQUIRE_EQ(archive.entries.size(), 5u);
    const auto files = by_path(archive);
    CHECK(test::str(files.at("usr/share/doc/cvt-demo/hello.txt")->bytes) == "Hello, archive!\n");
    CHECK(files.at("usr/share/doc/cvt-demo/lorem.txt")->bytes == lorem());
    CHECK(test::str(files.at("usr/bin/cvt-demo")->bytes) == "#!/bin/sh\necho hello\n");
    CHECK_EQ(files.at("usr/bin/cvt-demo")->unix_mode, 0755u);
    CHECK_EQ(files.at("usr/bin/cvt-demo")->mtime, tree::mtime);
    CHECK(files.at("usr/share/doc/cvt-demo")->is_directory);
    CHECK(files.at("usr/share/doc/cvt-demo/link")->symlink_target == "hello.txt");
    CHECK(detected(unix_fixture(name)) == "rpm");
  }
  // rpm -> cpio -> back.
  const test::Converted cpio = test::convert(unix_fixture("demo_gz.rpm"), "", "cpio");
  REQUIRE(cpio.ok());
  CHECK_EQ(test::read_as<ir::Archive>("cpio", cpio.data)->entries.size(), 9u);  // + the parent directories usr, usr/bin, usr/share, usr/share/doc
}

// ------------------------------------------------------------------------------- xar

TEST_CASE("archive/xar: bsdtar archives with zlib, xz and bzip2 encodings") {
  // hello.txt, lorem.txt, dir (with nested/deep/file.txt and empty_dir), exec.sh.
  const ir::Archive archive = read_ok("xar", "tree.xar");
  const auto all = by_path(archive);
  REQUIRE_EQ(archive.entries.size(), 8u);
  for (const tree::File& f : tree::files()) {
    if (!all.contains(f.path)) continue;
    INFO("entry ", f.path);
    CHECK(all.at(f.path)->bytes == f.bytes);
    CHECK_EQ(all.at(f.path)->unix_mode, f.mode);
    CHECK_EQ(all.at(f.path)->mtime, tree::mtime);
  }
  for (const char* name : {"hello.txt", "lorem.txt", "exec.sh", "dir/nested/deep/file.txt"}) CHECK(all.contains(name));
  for (const char* dir : {"dir", "dir/nested", "dir/nested/deep", "dir/empty_dir"}) {
    INFO("directory ", dir);
    REQUIRE(all.contains(dir));
    CHECK(all.at(dir)->is_directory);
    CHECK_EQ(all.at(dir)->unix_mode, 0755u);
  }
  const ir::Archive xz = read_ok("xar", "tree_xz_md5.xar");
  const auto files = by_path(xz);
  REQUIRE_EQ(xz.entries.size(), 3u);
  CHECK(files.at("lorem.txt")->bytes == lorem());
  CHECK(files.at("link")->symlink_target == "hello.txt");
  const ir::Archive bz2 = read_ok("xar", "tree_bz2.xar");
  REQUIRE_EQ(bz2.entries.size(), 2u);
  CHECK(by_path(bz2).at("noise.bin")->bytes == tree::noise(4096));
  CHECK(detected(unix_fixture("tree.xar"), "Install.pkg") == "xar");
}

TEST_CASE("archive/xar: table of contents and file checksums are verified") {
  // A changed heap byte inside hello.txt's zlib data.
  Bytes data = unix_fixture("tree.xar");
  const std::size_t heap = 28 + 876;
  Bytes changed = data;
  changed[heap + 20 + 5] ^= std::byte{0x01};
  CHECK_STATUS(test::read_as<ir::Archive>("xar", changed), Status::Decode);
  // A changed TOC checksum.
  Bytes toc = data;
  toc[heap] ^= std::byte{0x01};
  CHECK_STATUS(test::read_as<ir::Archive>("xar", toc), Status::Decode);
}

// ------------------------------------------------------------------------------- warc

TEST_CASE("archive/warc: response and resource records become files") {
  for (const char* name : {"demo.warc", "demo.warc.gz"}) {
    INFO("fixture ", name);
    std::vector<std::string> warnings;
    const ir::Archive archive = read_ok("warc", name, &warnings);
    CHECK(warnings.empty());
    const auto files = by_path(archive);
    REQUIRE_EQ(archive.entries.size(), 6u);
    CHECK(test::str(files.at("example.com/index.html")->bytes) ==
          "<!doctype html><title>Demo</title><p>Hello, archive!</p>\n");
    CHECK(test::str(files.at("example.com/docs/page.html")->bytes) == "<!doctype html><title>Page</title><p>A page.</p>\n");
    CHECK(test::str(files.at("example.com/docs/page~2.html")->bytes) ==
          "<!doctype html><title>Page</title><p>A newer page.</p>\n");
    CHECK(files.at("example.com/lorem.txt")->bytes == lorem());  // chunked
    CHECK(test::str(files.at("example.org/hello.txt")->bytes) == "Hello, archive!\n");  // gzip content coding
    CHECK(test::str(files.at("example.org/a/b/index.html")->bytes) == "resource body\n");
    CHECK_EQ(files.at("example.org/hello.txt")->mtime, tree::mtime);
    CHECK(detected(unix_fixture(name), name) == "warc");
    CHECK(detected(unix_fixture(name)) == "warc");
  }
  const test::Converted zip = test::convert(unix_fixture("demo.warc.gz"), "", "zip", "", "demo.warc.gz");
  REQUIRE(zip.ok());
  CHECK_EQ(test::read_as<ir::Archive>("zip", zip.data)->entries.size(), 6u);
}

TEST_CASE("archive/warc: hostile target URIs stay inside the archive") {
  const auto record = [](std::string_view uri, std::string_view body) {
    return std::format("WARC/1.1\r\nWARC-Type: resource\r\nWARC-Target-URI: {}\r\nContent-Length: {}\r\n\r\n{}\r\n\r\n", uri,
                       body.size(), body);
  };
  const std::string warc = record("http://evil.example/../../etc/passwd", "a") + record("http://evil.example/%2e%2e/%2E%2E/x", "b") +
                           record("http://evil.example/a%2Fb/c:d?e", "c") + record("file:///C:/Windows/x.ini", "d") +
                           record("urn:x-demo:thing", "e") + record("http://user:pw@Evil.Example:8080", "f");
  const auto archive = test::read_as<ir::Archive>("warc", as_bytes(warc));
  REQUIRE_OK(archive);
  const auto files = by_path(*archive);
  CHECK(files.contains("evil.example/etc/passwd"));
  CHECK(files.contains("evil.example/x"));
  CHECK(files.contains("evil.example/a_b/c_d"));
  CHECK(files.contains("unknown/C_/Windows/x.ini"));
  CHECK(files.contains("urn/x-demo_thing"));
  CHECK(files.contains("evil.example_8080/index.html"));
  CHECK_EQ(archive->entries.size(), 6u);
}

// ------------------------------------------------------------------------------- shar

TEST_CASE("archive/shar: the script recreates the tree with here-documents and base64") {
  ir::Archive source = tree::archive();
  source.entries.push_back({"no_newline.txt", test::bytes("no newline at the end"), false, tree::mtime, 0600, {}});
  source.entries.push_back({"it's quoted.txt", test::bytes("SHAR_EOF\n$HOME `x`\n"), false, tree::mtime, 0644, {}});
  const auto written = test::write_from("shar", source, R"({"strip_metadata":false})");
  REQUIRE_OK(written);
  const std::string script = test::str(*written);
  CHECK(script.starts_with("#!/bin/sh\n"));
  CHECK(script.ends_with("exit 0\n"));
  CHECK(script.find("mkdir -p './dir/empty_dir'\n") != std::string::npos);
  CHECK(script.find("mkdir -p './dir/nested/deep'\n") != std::string::npos);
  CHECK(script.find("LC_ALL=C sed 's/^X//' > './hello.txt' << 'SHAR_EOF'\nXHello, archive!\nSHAR_EOF\nchmod 644 './hello.txt'\n") !=
        std::string::npos);
  CHECK(script.find("chmod 755 './exec.sh'\n") != std::string::npos);
  CHECK(script.find(": > './empty.txt'\n") != std::string::npos);
  CHECK(script.find("ln -sf 'hello.txt' './link'\n") != std::string::npos);
  CHECK(script.find("'./it'\\''s quoted.txt'") != std::string::npos);
  CHECK(script.find("XSHAR_EOF\nX$HOME `x`\nSHAR_EOF\n") != std::string::npos);
  CHECK(script.find("chmod 600 './no_newline.txt'") != std::string::npos);
  // Binary and newline-less files are base64; decoding the here-document gives the bytes back.
  const auto base64_of = [&](std::string_view path) {
    const std::string head = std::format("base64 -d > '{}' << 'SHAR_EOF'\n", path);
    const std::size_t at = script.find(head);
    REQUIRE(at != std::string::npos);
    const std::size_t start = at + head.size();
    const std::size_t end = script.find("SHAR_EOF\n", start);
    auto decoded = util::base64_decode(std::string_view(script).substr(start, end - start));
    REQUIRE_OK(decoded);
    return *decoded;
  };
  CHECK(base64_of("./noise.bin") == tree::noise(4096));
  CHECK(test::str(base64_of("./no_newline.txt")) == "no newline at the end");
  // Every text line is X-prefixed: the delimiter appears only as a terminator.
  std::size_t delimiters = 0;
  for (const std::string_view line : util::split(script, '\n')) {
    if (line == "SHAR_EOF") ++delimiters;
  }
  std::size_t openers = 0;
  for (std::size_t at = script.find("<< 'SHAR_EOF'"); at != std::string::npos; at = script.find("<< 'SHAR_EOF'", at + 1)) {
    ++openers;
  }
  CHECK_EQ(delimiters, openers);
  // Deterministic, and no owner information.
  CHECK(*test::write_from("shar", source, R"({"strip_metadata":false})") == *written);
  CHECK(script.find("chown") == std::string::npos);
  // Through the engine: a tar becomes a shell archive.
  const test::Converted converted = test::convert(test::fixture("archive/tar/tree_gnu.tar"), "tar", "shar");
  REQUIRE(converted.ok());
  CHECK(converted.text().find("'./long/" + tree::long_name() + "'") != std::string::npos);
}

// ------------------------------------------------------------------------------- shared

TEST_CASE("archive/unix: detection by content and by name") {
  CHECK(detected(unix_fixture("tree_newc.cpio")) == "cpio");
  CHECK(detected(unix_fixture("tree_odc.cpio")) == "cpio");
  CHECK(detected(unix_fixture("tree_bin.cpio")) == "cpio");
  CHECK(detected(unix_fixture("tree.a")) == "ar");
  CHECK(detected(unix_fixture("tree_bsd.a"), "libdemo.a") == "ar");
  CHECK(detected(unix_fixture("demo_gz.deb"), "demo.a") == "deb");
  CHECK(detected(unix_fixture("demo_gz.rpm")) == "rpm");
  CHECK(detected(unix_fixture("tree.xar")) == "xar");
  CHECK(detected(unix_fixture("demo.warc")) == "warc");
  // A gzip of something else is not a WARC, a tar is not a cpio.
  const auto gz = compression::compress(Method::Gzip, test::bytes("plain text, not a web archive\n"));
  REQUIRE_OK(gz);
  CHECK(detected(*gz) != "warc");
  CHECK(detected(test::fixture("archive/tar/tree_gnu.tar")) == "tar");
}

TEST_CASE("archive/unix: conversions between the formats") {
  // cpio -> zip -> cpio keeps the tree.
  const test::Converted zip = test::convert(unix_fixture("tree_newc.cpio"), "", "zip");
  REQUIRE(zip.ok());
  const test::Converted cpio = test::convert(zip.data, "zip", "cpio");
  REQUIRE(cpio.ok());
  tree::check(*test::read_as<ir::Archive>("cpio", cpio.data));
  // Same format: cpio -> cpio rewrites as newc.
  const test::Converted again = test::convert(unix_fixture("tree_odc.cpio"), "", "cpio");
  REQUIRE(again.ok());
  CHECK(test::str(ByteView(again.data).first(6)) == "070701");
  tree::check(*test::read_as<ir::Archive>("cpio", again.data));
  // ar -> tar.
  const test::Converted tar = test::convert(unix_fixture("tree.a"), "", "tar");
  REQUIRE(tar.ok());
  CHECK_EQ(test::read_as<ir::Archive>("tar", tar.data)->entries.size(), 5u);
  // xar -> ar.
  const test::Converted ar = test::convert(unix_fixture("tree.xar"), "", "ar");
  REQUIRE(ar.ok());
  CHECK(by_path(*test::read_as<ir::Archive>("ar", ar.data)).contains("file.txt"));
}

TEST_CASE("archive/unix: damaged input fails cleanly") {
  test::fuzz_reader<ir::Archive>("cpio", unix_fixture("tree_newc.cpio"));
  test::fuzz_reader<ir::Archive>("cpio", unix_fixture("tree_crc.cpio"));
  test::fuzz_reader<ir::Archive>("cpio", unix_fixture("tree_odc.cpio"));
  test::fuzz_reader<ir::Archive>("cpio", unix_fixture("tree_bin.cpio"));
  test::fuzz_reader<ir::Archive>("ar", unix_fixture("tree.a"));
  test::fuzz_reader<ir::Archive>("ar", unix_fixture("tree_bsd_long.a"));
  test::fuzz_reader<ir::Archive>("deb", unix_fixture("demo_gz.deb"));
  test::fuzz_reader<ir::Archive>("rpm", unix_fixture("demo_gz.rpm"));
  test::fuzz_reader<ir::Archive>("xar", unix_fixture("tree.xar"));
  test::fuzz_reader<ir::Archive>("warc", unix_fixture("demo.warc"));
  test::fuzz_reader<ir::Archive>("warc", unix_fixture("demo.warc.gz"));
  for (const char* format : {"cpio", "ar", "deb", "rpm", "xar", "warc"}) {
    INFO("format ", format);
    CHECK_STATUS(test::read_as<ir::Archive>(format, test::random_bytes(2048, 5)), Status::Decode);
  }
  // Every prefix of the small fixtures.
  const Bytes small = unix_fixture("tree_bsd_long.a");
  for (std::size_t n = 0; n < small.size(); ++n) (void)test::read_as<ir::Archive>("ar", ByteView(small).first(n));
  const Bytes deb = unix_fixture("demo_gz.deb");
  for (std::size_t n = 0; n < deb.size(); ++n) (void)test::read_as<ir::Archive>("deb", ByteView(deb).first(n));
}

TEST_CASE("archive/unix: limits") {
  Limits few;
  few.max_archive_entries = 3;
  CHECK_STATUS(test::read_as<ir::Archive>("cpio", unix_fixture("tree_newc.cpio"), "", nullptr, few), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("ar", unix_fixture("tree.a"), "", nullptr, few), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("xar", unix_fixture("tree.xar"), "", nullptr, few), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("warc", unix_fixture("demo.warc"), "", nullptr, few), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("rpm", unix_fixture("demo_gz.rpm"), "", nullptr, few), Status::Limit);
  Limits tiny;
  tiny.max_decompressed_bytes = 1000;
  CHECK_STATUS(test::read_as<ir::Archive>("cpio", unix_fixture("tree_newc.cpio"), "", nullptr, tiny), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("xar", unix_fixture("tree.xar"), "", nullptr, tiny), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("rpm", unix_fixture("demo_gz.rpm"), "", nullptr, tiny), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("deb", unix_fixture("demo_gz.deb"), "", nullptr, tiny), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Archive>("warc", unix_fixture("demo.warc.gz"), "", nullptr, tiny), Status::Limit);
  // A gzip bomb inside a deb data member.
  const auto zeros = compression::compress(Method::Gzip, Bytes(8u << 20, std::byte{0}));
  REQUIRE_OK(zeros);
  std::string deb = "!<arch>\n";
  const auto member = [&](std::string_view name, ByteView body) {
    deb += std::format("{:<16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n", name, 0, 0, 0, 100644, body.size());
    deb += as_chars(body);
    if (body.size() % 2 != 0) deb += '\n';
  };
  member("debian-binary", as_bytes("2.0\n"));
  member("data.tar.gz", *zeros);
  Limits small;
  small.max_decompressed_bytes = 1u << 20;
  CHECK_STATUS(test::read_as<ir::Archive>("deb", as_bytes(deb), "", nullptr, small), Status::Limit);
}
