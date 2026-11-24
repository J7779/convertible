// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <filesystem>
#include <string>
#include <string_view>

#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/fs.hpp"

using namespace cvt;

TEST_CASE("support: fixture names are validated") {
  for (const std::string_view good : {"image/gradient_8x8.png", "support/hello.txt", "a", "a/b/c.d", "..a/b..",
                                      "family/name with space.bin", "audio/\xC3\xA9t\xC3\xA9.wav"}) {
    CHECK_MESSAGE(test::is_valid_fixture_name(good), "should accept: ", good);
  }
  for (const std::string_view bad :
       {"", "/", "/etc/hosts", "//server/share", "..", "../x", "a/../b", "a/..", ".", "./a", "a/./b", "a//b", "a/",
        "a\\b", "..\\x", "C:/x", "C:x", "c:\\x", "a/b:c"}) {
    CHECK_MESSAGE(!test::is_valid_fixture_name(bad), "should reject: ", bad);
  }
  CHECK_FALSE(test::is_valid_fixture_name(std::string_view("a\0b", 3)));
  static_assert(test::is_valid_fixture_name("support/hello.txt"));
  static_assert(!test::is_valid_fixture_name("../secret"));
}

TEST_CASE("support: fixture_path stays inside the data directory") {
  const std::filesystem::path path = test::fixture_path("support/nested/crlf.txt");
  CHECK(path.is_absolute());
  CHECK(util::filename_utf8(path) == "crlf.txt");
  CHECK(util::filename_utf8(path.parent_path()) == "nested");
  CHECK(util::filename_utf8(path.parent_path().parent_path()) == "support");
  CHECK(path.parent_path().parent_path().parent_path() == test::fixture_path("support").parent_path());
  // A path is produced for files that do not exist, too.
  CHECK(util::filename_utf8(test::fixture_path("support/not-there.bin")) == "not-there.bin");
}

TEST_CASE("support: has_fixture reports regular files only") {
  CHECK(test::has_fixture("support/hello.txt"));
  CHECK(test::has_fixture("support/bytes.bin"));
  CHECK(test::has_fixture("support/nested/crlf.txt"));
  CHECK_FALSE(test::has_fixture("support/not-there.bin"));
  CHECK_FALSE(test::has_fixture("no-such-family/file.bin"));
  CHECK_FALSE(test::has_fixture("support"));         // a directory
  CHECK_FALSE(test::has_fixture("support/nested"));  // a directory
}

TEST_CASE("support: fixture loads binary files byte for byte") {
  const Bytes data = test::fixture("support/bytes.bin");
  const Bytes expected = {std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x7F},
                          std::byte{0x80}, std::byte{0xFE}, std::byte{0xFF}, std::byte{0x0A}};
  CHECK(data == expected);
  CHECK(test::fixture("support/hello.txt") == test::bytes("Hello, fixture!\n"));
  // Loading twice gives the same bytes: nothing is cached or consumed.
  CHECK(test::fixture("support/bytes.bin") == data);
}

TEST_CASE("support: fixture_text returns the file unchanged") {
  CHECK(test::fixture_text("support/hello.txt") == "Hello, fixture!\n");
  // No line-ending translation, on any platform.
  CHECK(test::fixture_text("support/nested/crlf.txt") == "line one\r\nline two\r\n");
  // Embedded NUL and non-UTF-8 bytes survive.
  const std::string raw = test::fixture_text("support/bytes.bin");
  CHECK(raw.size() == 8);
  CHECK(raw == std::string("\x00\x01\x02\x7F\x80\xFE\xFF\n", 8));
}
