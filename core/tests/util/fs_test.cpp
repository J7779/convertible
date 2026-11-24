// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <filesystem>

#include "support/test_support.hpp"
#include "util/fs.hpp"
#include "util/mapped_file.hpp"

using namespace cvt;
using namespace cvt::util;

TEST_CASE("fs: UTF-8 paths round-trip") {
  const std::string name = "d\xC3\xA9j\xC3\xA0 vu/\xE2\x82\xAC\xF0\x9F\x98\x80.txt";
  const std::filesystem::path path = path_from_utf8(name);
  CHECK(path_to_utf8(path.filename()) == "\xE2\x82\xAC\xF0\x9F\x98\x80.txt");
  CHECK(filename_utf8(path) == "\xE2\x82\xAC\xF0\x9F\x98\x80.txt");
  CHECK(path_to_utf8(path_from_utf8("plain.bin")) == "plain.bin");
  CHECK(path_from_utf8("").empty());
  // Invalid UTF-8 is repaired rather than passed to the OS as garbage.
  CHECK(path_to_utf8(path_from_utf8("a\xFF" "b")) == "a\xEF\xBF\xBD" "b");
}

TEST_CASE("fs: TempDir creates a private directory and removes it") {
  std::filesystem::path location;
  {
    auto temp = TempDir::create("cvt-test");
    REQUIRE(temp);
    location = temp->path();
    CHECK(std::filesystem::is_directory(location));
    CHECK(path_to_utf8(location.filename()).starts_with("cvt-test-"));
    REQUIRE(write_file(location / "nested.bin", as_bytes("x")));

    auto second = TempDir::create("cvt-test");
    REQUIRE(second);
    CHECK(second->path() != location);

    TempDir moved = std::move(*temp);
    CHECK(moved.path() == location);
    CHECK(std::filesystem::exists(location));
  }
  CHECK_FALSE(std::filesystem::exists(location));
}

TEST_CASE("fs: read_file and write_file") {
  auto temp = TempDir::create("cvt-test");
  REQUIRE(temp);
  const std::filesystem::path file = temp->path() / path_from_utf8("donn\xC3\xA9" "es \xE2\x82\xAC.bin");
  const Bytes data = test::random_bytes(200000, 21);

  REQUIRE(write_file(file, data));
  CHECK(std::filesystem::file_size(file) == data.size());
  CHECK(*read_file(file) == data);
  CHECK(*read_file(file, data.size()) == data);
  CHECK(read_file(file, data.size() - 1).error().code == Status::Limit);
  CHECK(*read_file_prefix(file, 16) == Bytes(data.begin(), data.begin() + 16));
  CHECK(read_file_prefix(file, 1 << 20)->size() == data.size());

  REQUIRE(write_file(file, as_bytes("short")));  // replaces, does not append
  CHECK(test::str(*read_file(file)) == "short");
  REQUIRE(write_file(file, {}));
  CHECK(read_file(file)->empty());

  CHECK(read_file(temp->path() / "missing.bin").error().code == Status::Io);
  CHECK(read_file(temp->path()).error().code == Status::Io);  // a directory
  CHECK(read_file_prefix(temp->path(), 4).error().code == Status::Io);
  CHECK(write_file(temp->path() / "no-such-dir" / "x.bin", as_bytes("x")).error().code == Status::Io);
  CHECK(write_file(temp->path(), as_bytes("x")).error().code == Status::Io);
}

TEST_CASE("fs: MappedFile exposes file contents without copying") {
  auto temp = TempDir::create("cvt-test");
  REQUIRE(temp);
  const std::filesystem::path file = temp->path() / "mapped.bin";
  const Bytes data = test::random_bytes(300000, 33);
  REQUIRE(write_file(file, data));

  auto mapped = MappedFile::open(file);
  REQUIRE(mapped);
  CHECK(mapped->size() == data.size());
  CHECK(std::ranges::equal(mapped->bytes(), data));
#if !defined(__EMSCRIPTEN__)
  CHECK(mapped->is_mapped());
#endif

  MappedFile moved = std::move(*mapped);
  CHECK(mapped->bytes().empty());
  CHECK(std::ranges::equal(moved.bytes(), data));
  MappedFile assigned;
  assigned = std::move(moved);
  CHECK(std::ranges::equal(assigned.bytes(), data));
  assigned.close();
  CHECK(assigned.bytes().empty());
  CHECK_FALSE(assigned.is_mapped());
  // Once released, the file can be replaced.
  REQUIRE(write_file(file, as_bytes("new")));

  // Empty files cannot be mapped but still open.
  REQUIRE(write_file(file, {}));
  auto empty = MappedFile::open(file);
  REQUIRE(empty);
  CHECK(empty->bytes().empty());

  REQUIRE(write_file(file, as_bytes("12345")));
  CHECK(MappedFile::open(file, 4).error().code == Status::Limit);
  CHECK(MappedFile::open(file, 5).has_value());
  CHECK(MappedFile::open(temp->path() / "missing").error().code == Status::Io);
  CHECK(MappedFile::open(temp->path()).error().code == Status::Io);
}
