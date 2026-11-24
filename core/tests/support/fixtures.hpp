// Test fixtures: real files under core/tests/data/<family>/<name>.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Usage, from inside a test case:
//
//   #include "support/fixtures.hpp"
//
//   const Bytes png = cvt::test::fixture("image/gradient_8x8.png");
//   auto image = cvt::test::read_as<ir::Image>("png", png);
//   REQUIRE(image);
//
// Names are always relative to the data directory, UTF-8, with forward
// slashes on every platform. The rules for what may be committed there (and
// the SOURCES.md every family directory needs) are in
// core/tests/data/README.md.
//
// A missing or unreadable fixture makes the calling test FAIL (it is never
// skipped silently); the failure is reported at the line of the call and
// names the fixture. The absolute location of the data directory is injected
// by the build (CVT_TEST_DATA_DIR) and is never part of a message.
#pragma once

#include <cstddef>
#include <filesystem>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "support/test.hpp"

#include "engine/errors.hpp"
#include "util/bytes.hpp"
#include "util/fs.hpp"

#ifndef CVT_TEST_DATA_DIR
#error "CVT_TEST_DATA_DIR is not defined: fixtures.hpp is only usable from the convertible_tests target"
#endif

namespace cvt::test {

/// True when `relative` is an acceptable fixture name: non-empty, relative,
/// '/'-separated, with no empty, "." or ".." segments and none of the
/// characters '\\', ':' or NUL (so a name can never leave the data directory
/// and means the same file on every platform).
///
///   "image/gradient_8x8.png"   ok
///   "/etc/hosts", "C:/x", "a/../b", "./a", "a//b", "a\\b", "a/"   rejected
[[nodiscard]] constexpr bool is_valid_fixture_name(std::string_view relative) noexcept {
  if (relative.empty()) return false;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = relative.find('/', start);
    const std::string_view segment =
        relative.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    for (const char c : segment) {
      if (c == '\\' || c == ':' || c == '\0') return false;
    }
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
}

namespace detail {

/// Record a non-fatal test failure attributed to the caller's line.
inline void fixture_failure(const std::source_location& caller, const std::string& message) {
  CVT_TEST_FAIL_AT(caller.file_name(), static_cast<int>(caller.line()), message);
}

/// Checks the name; reports a failure and returns false when it is invalid.
[[nodiscard]] inline bool check_fixture_name(std::string_view relative, const std::source_location& caller) {
  if (is_valid_fixture_name(relative)) return true;
  fixture_failure(caller, "invalid fixture name '" + std::string(relative) +
                              "': use a relative path below core/tests/data/ with '/' separators and no '.' or "
                              "'..' segments");
  return false;
}

/// Data directory / relative, for a name that already passed validation.
[[nodiscard]] inline std::filesystem::path fixture_path_unchecked(std::string_view relative) {
  std::filesystem::path path = util::path_from_utf8(CVT_TEST_DATA_DIR);
  std::size_t start = 0;
  while (start <= relative.size()) {
    const std::size_t end = relative.find('/', start);
    path /= util::path_from_utf8(
        relative.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return path;
}

}  // namespace detail

/// Absolute path of the fixture `relative` (e.g. "audio/sine_440.wav"), for
/// the rare test that needs a file on disk rather than its bytes. The file
/// need not exist. An invalid name (see is_valid_fixture_name) fails the
/// calling test and yields an empty path. Do not put the result in a failure
/// message or a committed file: it contains the local checkout location.
[[nodiscard]] inline std::filesystem::path fixture_path(
    std::string_view relative, std::source_location caller = std::source_location::current()) {
  if (!detail::check_fixture_name(relative, caller)) return {};
  return detail::fixture_path_unchecked(relative);
}

/// True when the fixture exists as a regular file. An invalid name fails the
/// calling test and yields false. Prefer calling fixture() directly: decode
/// tests must not be skipped because a file is absent.
[[nodiscard]] inline bool has_fixture(std::string_view relative,
                                      std::source_location caller = std::source_location::current()) {
  if (!detail::check_fixture_name(relative, caller)) return false;
  std::error_code ec;
  return std::filesystem::is_regular_file(detail::fixture_path_unchecked(relative), ec);
}

/// The contents of the fixture `relative`, byte for byte. When the name is
/// invalid, or the file is missing or unreadable, the calling test fails
/// (non-fatally, like CHECK) with a message naming the fixture, and empty
/// bytes are returned; write `REQUIRE(!data.empty())` to stop the test there.
[[nodiscard]] inline Bytes fixture(std::string_view relative,
                                   std::source_location caller = std::source_location::current()) {
  if (!detail::check_fixture_name(relative, caller)) return {};
  const std::filesystem::path path = detail::fixture_path_unchecked(relative);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    detail::fixture_failure(caller, "fixture '" + std::string(relative) +
                                        "' not found below core/tests/data/ (is the file committed and listed in "
                                        "its SOURCES.md?)");
    return {};
  }
  auto data = util::read_file(path);
  if (!data) {
    // The error text of read_file may contain the absolute path; report only its category.
    detail::fixture_failure(caller, "fixture '" + std::string(relative) + "' could not be read (" +
                                        std::string(status_name(data.error().code)) + ")");
    return {};
  }
  return std::move(*data);
}

/// fixture() as a string, byte for byte: no decoding, no byte-order-mark
/// removal, no line-ending translation. Fails the calling test and returns
/// "" under the same conditions as fixture().
[[nodiscard]] inline std::string fixture_text(std::string_view relative,
                                              std::source_location caller = std::source_location::current()) {
  return to_string(ByteView(fixture(relative, caller)));
}

}  // namespace cvt::test
