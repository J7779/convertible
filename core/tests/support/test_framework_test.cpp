// Tests of the in-house test framework itself (support/test.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Failing assertions are exercised inside cvt::test::detail::run_isolated,
// which runs a body like a test case but with its own counters and captured
// output, so nothing here fails or prints in the real run.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "support/test.hpp"

#include "engine/errors.hpp"

#if defined(CVT_TEST_DATA_DIR)
#  include "support/fixtures.hpp"
#endif

namespace fwt {

enum class Plain { A, B };
enum class Named { Red, Blue = 2 };
inline std::ostream& operator<<(std::ostream& out, Named value) {
  return out << (value == Named::Red ? "Red" : "Blue");
}
enum Unscoped { UnscopedA, UnscopedB };

/// Comparable, but with no way to print it.
struct OnlyEq {
  int value;
  bool operator==(const OnlyEq&) const = default;
};

/// Printed through operator<<, found by argument-dependent lookup.
struct Streamed {
  int value;
  bool operator==(const Streamed&) const = default;
};
inline std::ostream& operator<<(std::ostream& out, const Streamed& streamed) {
  return out << "Streamed<" << streamed.value << ">";
}

/// Printed through a Printer specialisation, which wins over operator<<.
struct Custom {
  int value;
  bool operator==(const Custom&) const = default;
};
inline std::ostream& operator<<(std::ostream& out, const Custom&) { return out << "never used"; }

/// Neither copyable nor movable.
struct Pinned {
  explicit Pinned(int v) : value(v) {}
  Pinned(const Pinned&) = delete;
  Pinned& operator=(const Pinned&) = delete;
  bool operator==(const Pinned& other) const { return value == other.value; }
  bool operator<(const Pinned& other) const { return value < other.value; }
  int value;
};

}  // namespace fwt

namespace cvt::test {
template <>
struct Printer<fwt::Custom> {
  static std::string to_string(const fwt::Custom& custom) { return "Custom#" + std::to_string(custom.value); }
};
}  // namespace cvt::test

namespace {

using cvt::Result;
using cvt::Status;
using cvt::test::approx;
using cvt::test::stringify;
using cvt::test::detail::IsolatedOptions;
using cvt::test::detail::IsolatedResult;
using cvt::test::detail::run_isolated;
namespace fw = cvt::test::fw;

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

std::size_t occurrences(std::string_view text, std::string_view part) {
  std::size_t count = 0;
  for (std::size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size())) ++count;
  return count;
}

/// The rest of the first line of `output` that starts with `prefix`, or "<none>".
std::string line_after(std::string_view output, std::string_view prefix) {
  std::size_t start = 0;
  while (start < output.size()) {
    const std::size_t end = std::min(output.find('\n', start), output.size());
    const std::string_view line = output.substr(start, end - start);
    if (line.starts_with(prefix)) return std::string(line.substr(prefix.size()));
    start = end + 1;
  }
  return "<none>";
}

/// What the first failing assertion of `body` printed after "with expansion: ".
template <class Body>
std::string expansion_of(Body&& body) {
  const IsolatedResult result = run_isolated(body);
  return line_after(result.output, "  with expansion: ");
}

// ---- static checks ---------------------------------------------------------

// Operands are held by reference; string literals decay to pointers so that
// every literal length does not instantiate its own comparison.
static_assert(std::is_same_v<decltype(fw::Decomposer{} <=> 1), fw::Lhs<const int&>>);
static_assert(std::is_same_v<decltype(fw::Decomposer{} <=> "abc"), fw::Lhs<const char*>>);
static_assert(std::is_same_v<decltype(fw::Decomposer{} <=> std::declval<const std::string&>() == "abc"),
                             fw::Binary<const std::string&, const char*>>);
static_assert(
    std::is_same_v<decltype(fw::Decomposer{} <=> std::declval<fwt::Pinned&>() == std::declval<fwt::Pinned&>()),
                   fw::Binary<const fwt::Pinned&, const fwt::Pinned&>>);
static_assert(std::is_same_v<decltype(fw::Decomposer{} <=> 1 + 2 < 4), fw::Binary<const int&, const int&>>);
static_assert(std::is_same_v<decltype(fw::Decomposer{} <=> 6 & 3), fw::Lhs<int>>);

// Tolerant comparison is usable in constant expressions.
static_assert(1.0 == approx(1.0));
static_assert(1.0 + 1e-10 == approx(1.0));
static_assert(!(1.0 + 1e-6 == approx(1.0)));
static_assert(1.05 == approx(1.0).abs(0.1));
static_assert(1.05 != approx(1.0).abs(0.01));
static_assert(104.0 == approx(100.0).rel(0.05));
static_assert(!(1e-3 == approx(0.0).rel(0.5)));  // a relative tolerance alone never reaches zero
static_assert(1e-13 == approx(0.0));             // ... the default has an absolute floor
static_assert(approx(1.0) == 1.0);
static_assert(3 == approx(3.0));
static_assert(2.0 <= approx(3.0) && 3.0000000001 <= approx(3.0) && !(3.1 <= approx(3.0)));
static_assert(4.0 >= approx(3.0) && approx(3.0) <= 4.0 && approx(3.0) >= 2.0);
static_assert(!(std::numeric_limits<double>::quiet_NaN() == approx(std::numeric_limits<double>::quiet_NaN())));
static_assert(!(1.0 == approx(std::numeric_limits<double>::quiet_NaN()).abs(1e300)));
static_assert(std::numeric_limits<double>::infinity() == approx(std::numeric_limits<double>::infinity()));
static_assert(!(-std::numeric_limits<double>::infinity() == approx(std::numeric_limits<double>::infinity())));
static_assert(!(1e308 == approx(std::numeric_limits<double>::infinity()).rel(1.0)));

}  // namespace

// ---- decomposition -----------------------------------------------------------

TEST_CASE("framework: expansion of integers, including mixed signedness") {
  CHECK(expansion_of([] { const int a = 1; CHECK(a == 2); }) == "1 == 2");
  CHECK(expansion_of([] { const long long a = -5; CHECK(a >= 0); }) == "-5 >= 0");
  CHECK(expansion_of([] { const std::vector<int> v{1, 2, 3}; CHECK(v.size() == 4); }) == "3 == 4");
  CHECK(expansion_of([] { const std::uint64_t a = 18446744073709551615ull; CHECK(a < 1u); }) ==
        "18446744073709551615 < 1");
  CHECK(expansion_of([] { const std::int8_t a = -3; const std::uint8_t b = 200; CHECK(a > b); }) == "-3 > 200");
  CHECK(expansion_of([] { const short a = 7; CHECK(a != 7); }) == "7 != 7");
  CHECK(expansion_of([] { const int a = 3; CHECK(a <= 2); }) == "3 <= 2");

  // The comparison is the one the compiler would do without the framework:
  // -1 converts to a large unsigned value here, exactly as in plain C++.
  const IsolatedResult usual = run_isolated([] {
    const unsigned u = 3;
    const int i = -1;
    CHECK(u < i);
    CHECK_FALSE(u > i);
  });
  CHECK(usual.passed == 2);
  CHECK(usual.failed == 0);
}

TEST_CASE("framework: expansion of floating point, bool and characters") {
  CHECK(expansion_of([] { const double d = 0.1; CHECK(d == 0.2); }) == "0.1 == 0.2");
  CHECK(expansion_of([] { const float f = 0.5f; CHECK(f == 0.25f); }) == "0.5f == 0.25f");
  CHECK(expansion_of([] { const float f = 0.5f; const double d = 0.1; CHECK(f < d); }) == "0.5f < 0.1");
  CHECK(expansion_of([] { const double d = 2.0; CHECK(d == 3); }) == "2.0 == 3");
  CHECK(expansion_of([] { const bool flag = true; CHECK(flag == false); }) == "true == false");
  CHECK(expansion_of([] { const char c = 'a'; CHECK(c == 'b'); }) == "'a' == 'b'");
  CHECK(expansion_of([] { const char c = '\n'; CHECK(c == '\''); }) == "'\\n' == '\\''");
  CHECK(expansion_of([] { const unsigned char c = 65; CHECK(c == 66); }) == "65 == 66");
  CHECK(expansion_of([] { const signed char c = -1; CHECK(c == 1); }) == "-1 == 1");
  CHECK(expansion_of([] { const std::byte b{0x01}; CHECK(b == std::byte{0xFF}); }) == "0x01 == 0xff");

  // A plain bool has nothing to expand.
  const IsolatedResult plain = run_isolated([] {
    const std::vector<int> v{1};
    CHECK(v.empty());
  });
  CHECK(plain.failed == 1);
  CHECK(contains(plain.output, "FAILED: CHECK( v.empty() )\n"));
  CHECK_FALSE(contains(plain.output, "with expansion"));
}

TEST_CASE("framework: expansion of strings") {
  CHECK(expansion_of([] { const std::string s = "01AB"; CHECK(s == "01ab"); }) == R"("01AB" == "01ab")");
  CHECK(expansion_of([] { const std::string s = "01AB"; CHECK("01ab" == s); }) == R"("01ab" == "01AB")");
  CHECK(expansion_of([] { const std::string_view v = "view"; const std::string s = "str"; CHECK(v == s); }) ==
        R"("view" == "str")");
  CHECK(expansion_of([] { const char* p = "ptr"; const std::string s = "str"; CHECK(p == s); }) == R"("ptr" == "str")");
  CHECK(expansion_of([] { const std::string s = "b"; CHECK(s < "a"); }) == R"("b" < "a")");
  CHECK(stringify(std::string("tab\t\"q\" \\ \x01\x7F\r\n")) == R"("tab\t\"q\" \\ \x01\x7f\r\n")");
  CHECK(stringify(std::string("nul\0in", 6)) == R"("nul\0in")");
  // Valid UTF-8 is kept, anything else is shown byte by byte.
  CHECK(stringify(std::string("\xC3\xA9t\xC3\xA9")) == "\"\xC3\xA9t\xC3\xA9\"");
  CHECK(stringify(std::string("bad \xFF\xC3 \xE2\x82")) == R"("bad \xff\xc3 \xe2\x82")");
  CHECK(stringify(std::string("\xC0\x80 \xED\xA0\x80")) == R"("\xc0\x80 \xed\xa0\x80")");  // overlong, surrogate
  // Character arrays stop at the first NUL or at their end.
  const char unterminated[3] = {'a', 'b', 'c'};
  CHECK(stringify(unterminated) == R"("abc")");
  const char padded[8] = "ab";
  CHECK(stringify(padded) == R"("ab")");
  const char* null_text = nullptr;
  CHECK(stringify(null_text) == "nullptr");
  CHECK(stringify(std::u8string(u8"h\u00E9")) == "\"h\xC3\xA9\"");
  CHECK(stringify(std::u16string(u"h\u00E9 \U0001F600")) == "\"h\xC3\xA9 \xF0\x9F\x98\x80\"");
  CHECK(stringify(std::u32string(U"h\u00E9")) == "\"h\xC3\xA9\"");
  CHECK(stringify(std::wstring(L"wide")) == R"("wide")");
  CHECK(stringify(U'\u00E9') == "U+00E9");
}

TEST_CASE("framework: expansion of enums, pointers and optionals") {
  CHECK(expansion_of([] { CHECK(fwt::Plain::A == fwt::Plain::B); }) == "0 == 1");
  CHECK(expansion_of([] { CHECK(fwt::Named::Red == fwt::Named::Blue); }) == "Red (0) == Blue (2)");
  CHECK(expansion_of([] { CHECK(fwt::UnscopedA == fwt::UnscopedB); }) == "0 == 1");
  CHECK(expansion_of([] { CHECK(Status::Ok == Status::Decode); }) == "ok == decode error");

  CHECK(expansion_of([] { const int* p = nullptr; CHECK(p != nullptr); }) == "nullptr != nullptr");
  const std::string address = expansion_of([] { const int value = 0; const int* p = &value; CHECK(p == nullptr); });
  CHECK(address.starts_with("0x"));
  CHECK(address.ends_with(" == nullptr"));
  CHECK(expansion_of([] { const std::unique_ptr<int> p; CHECK(p); }) == "nullptr");
  CHECK(stringify(std::shared_ptr<int>()) == "nullptr");
  void (*function)() = nullptr;
  CHECK(stringify(function) == "nullptr");

  CHECK(expansion_of([] { const std::optional<int> o; CHECK(o == 5); }) == "nullopt == 5");
  CHECK(expansion_of([] { const std::optional<int> o = 5; CHECK(o == std::nullopt); }) == "5 == std::nullopt_t");
  CHECK(expansion_of([] { const std::optional<int> o; CHECK(o); }) == "nullopt");
  CHECK(expansion_of([] { const std::optional<std::string> o = "x"; CHECK(o == std::optional<std::string>()); }) ==
        R"("x" == nullopt)");
}

TEST_CASE("framework: expansion of containers and byte runs") {
  const IsolatedResult vectors = run_isolated([] {
    const std::vector<int> v{1, 2, 3};
    CHECK(v == std::vector<int>{1, 2, 4});
  });
  CHECK(line_after(vectors.output, "  with expansion: ") == "{1, 2, 3} == {1, 2, 4}");
  CHECK(contains(vectors.output, "\n  first difference at index 2: 3 != 4\n"));

  const IsolatedResult sizes = run_isolated([] {
    const std::array<int, 3> a{1, 2, 3};
    const std::array<int, 3> b{1, 2, 3};
    const std::vector<std::string> names{"a", "b"};
    CHECK(a != b);
    CHECK(names == std::vector<std::string>{"a"});
  });
  CHECK(sizes.failed == 2);
  CHECK(contains(sizes.output, "  with expansion: {1, 2, 3} != {1, 2, 3}\n"));
  CHECK(contains(sizes.output, "  with expansion: {\"a\", \"b\"} == {\"a\"}\n"));
  CHECK(contains(sizes.output, "  sizes differ: 2 and 1 elements; the first 1 are equal\n"));

  const IsolatedResult bytes = run_isolated([] {
    const std::vector<std::byte> a{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> b{std::byte{1}, std::byte{0xFF}, std::byte{3}};
    CHECK(a == b);
  });
  CHECK(line_after(bytes.output, "  with expansion: ") == "bytes[3]{01 02 03} == bytes[3]{01 ff 03}");
  CHECK(contains(bytes.output, "  first difference at index 1 (0x1); sizes 3 and 3\n"
                               "    lhs[0...]: 01 02 03\n"
                               "    rhs[0...]: 01 ff 03\n"
                               "                  ^^\n"));

  const int raw[3] = {4, 5, 6};
  CHECK(stringify(raw) == "{4, 5, 6}");
  CHECK(stringify(std::span<const int>(raw)) == "{4, 5, 6}");
  CHECK(stringify(std::vector<std::vector<int>>{{1}, {}, {2, 3}}) == "{{1}, {}, {2, 3}}");
  CHECK(stringify(std::vector<int>()) == "{}");
  CHECK(stringify(std::vector<int>(40, 7)) == "{7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, ... (40 elements)}");
  CHECK(stringify(std::vector<bool>{true, false}).size() > 2);
  CHECK(stringify(std::map<std::string, int>{{"a", 1}, {"b", 2}}) == R"({{"a", 1}, {"b", 2}})");
  CHECK(stringify(std::vector<std::uint8_t>{0, 128, 255}) == "{0, 128, 255}");
  CHECK(stringify(std::vector<std::byte>()) == "bytes[0]{}");
  const std::vector<std::byte> many(40, std::byte{0xAB});
  CHECK(stringify(many) == "bytes[40]{ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab "
                           "ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ab ...}");
  CHECK(stringify(std::span<const std::byte>(many).first(2)) == "bytes[2]{ab ab}");
}

TEST_CASE("framework: expansion of results, tuples and other library types") {
  CHECK(expansion_of([] { const Result<int> r = cvt::fail(Status::Decode, "boom"); CHECK(r); }) ==
        R"(error{decode error, "boom"})");
  CHECK(expansion_of([] { const Result<int> r = 7; CHECK_FALSE(r); }) == "7");
  CHECK(expansion_of([] { const Result<void> r = cvt::fail(Status::Io, "disk"); CHECK(r); }) ==
        R"(error{I/O error, "disk"})");
  CHECK(stringify(Result<void>()) == "{void}");
  CHECK(stringify(std::expected<int, std::string>(std::unexpect, "why")) == R"(error{"why"})");
  CHECK(stringify(cvt::Error{Status::Limit, "too big"}) == R"({limit exceeded, "too big"})");
  const IsolatedResult has_value = run_isolated([] {
    const Result<int> r = cvt::fail(Status::Decode, "boom");
    CHECK(r.has_value());
    REQUIRE(r.has_value());
  });
  CHECK(has_value.failed == 2);

  CHECK(expansion_of([] { CHECK(std::pair<int, int>{1, 2} == std::make_pair(1, 3)); }) == "{1, 2} == {1, 3}");
  CHECK(stringify(std::tuple<int, std::string, double>{1, "a", 2.5}) == R"({1, "a", 2.5})");
  CHECK(stringify(std::tuple<>()) == "{}");
  CHECK(stringify(std::variant<int, std::string>(std::string("x"))) == R"("x")");
  CHECK(stringify(std::filesystem::path("dir") / "file.txt") == R"("dir/file.txt")");
  CHECK(stringify(std::chrono::milliseconds(5)) == "5 ms");
  CHECK(stringify(std::chrono::duration<double>(1.5)) == "1.5 s");
  CHECK(stringify(std::chrono::hours(2)) == "2 h");
  CHECK(stringify(std::chrono::duration<int, std::ratio<1, 30>>(3)) == "3 * 1/30 s");
  CHECK(stringify(nullptr) == "nullptr");
}

TEST_CASE("framework: user-defined types print through operator<<, Printer, or as a placeholder") {
  CHECK(expansion_of([] { CHECK(fwt::Streamed{1} == fwt::Streamed{2}); }) == "Streamed<1> == Streamed<2>");
  CHECK(expansion_of([] { CHECK(fwt::Custom{1} == fwt::Custom{2}); }) == "Custom#1 == Custom#2");
  CHECK(stringify(std::vector<fwt::Custom>{{1}, {2}}) == "{Custom#1, Custom#2}");
  CHECK(stringify(std::optional<fwt::Streamed>(fwt::Streamed{3})) == "Streamed<3>");

  const std::string unknown = expansion_of([] { CHECK(fwt::OnlyEq{1} == fwt::OnlyEq{2}); });
  CHECK(unknown.starts_with("{? "));
  CHECK(contains(unknown, "OnlyEq} == {? "));

  // Operands are never copied or moved.
  const IsolatedResult pinned = run_isolated([] {
    const fwt::Pinned a(1);
    fwt::Pinned b(2);
    CHECK(a == b);
    CHECK(a < b);
    CHECK(fwt::Pinned(3) == fwt::Pinned(3));
    REQUIRE_FALSE(b < a);
  });
  CHECK(pinned.passed == 3);
  CHECK(pinned.failed == 1);
}

TEST_CASE("framework: number formatting round-trips and stays readable") {
  CHECK(stringify(0) == "0");
  CHECK(stringify(-42) == "-42");
  CHECK(stringify(std::numeric_limits<std::int64_t>::min()) == "-9223372036854775808");
  CHECK(stringify(std::numeric_limits<std::uint64_t>::max()) == "18446744073709551615");
  CHECK(stringify(true) == "true");
  CHECK(stringify(1.0) == "1.0");
  CHECK(stringify(-0.5) == "-0.5");
  CHECK(stringify(0.1) == "0.1");
  CHECK(stringify(1.0 / 3.0) == "0.3333333333333333");
  CHECK(stringify(100000.0) == "100000.0");
  CHECK(stringify(123456789.125) == "123456789.125");
  CHECK(stringify(0.00001) == "0.00001");
  CHECK(stringify(1e300) == "1e+300");
  CHECK(stringify(2.5e-9) == "2.5e-09");
  CHECK(stringify(0.1f) == "0.1f");
  CHECK(stringify(16777216.0f) == "16777216.0f");
  CHECK(stringify(1.5L) == "1.5L");
  CHECK(stringify(std::numeric_limits<double>::quiet_NaN()) == "nan");
  CHECK(stringify(std::numeric_limits<double>::infinity()) == "inf");
  CHECK(stringify(-std::numeric_limits<float>::infinity()) == "-inf");
  CHECK(stringify(std::numeric_limits<double>::max()) == "1.7976931348623157e+308");
  CHECK(stringify(std::numeric_limits<double>::denorm_min()) == "5e-324");
}

TEST_CASE("framework: logical operators, commas, casts and bit operations inside CHECK") {
  int evaluated = 0;
  const auto touch = [&evaluated] {
    ++evaluated;
    return true;
  };
  const IsolatedResult result = run_isolated([&] {
    const int x = 3;
    const double d = 0.5;
    CHECK((x == 3 && d > 1.0));  // fails
    CHECK(x == 3 && d > 1.0);    // fails
    CHECK(x == 4 || d > 1.0);    // fails
    CHECK(x == 3 || touch());    // passes; short-circuit: touch() is not called
    CHECK(x == 4 && touch());    // fails; short-circuit again
    CHECK((x == 3 && d < 1.0) || touch());
    CHECK(x == 3 ? d < 1.0 : touch());
    CHECK(static_cast<int>(d) == 0);
    CHECK(std::pair<int, int>{1, 2} == std::pair<int, int>(1, 2));
    CHECK(std::vector<int>{1, 2}.size() == 2);
    CHECK(x & 1);
    CHECK_FALSE(x & 4);
    CHECK((x | 4) == 7);
    CHECK(x >> 1 == 1);
    CHECK(x + 1 == 4);
    CHECK(-x < 0);
    CHECK(!false);
  });
  CHECK(result.failed == 4);
  CHECK(result.passed == 13);
  CHECK(evaluated == 0);
  CHECK(expansion_of([] { const int x = 3; CHECK(x & 4); }) == "0");
  CHECK(expansion_of([] { const int x = 3; CHECK(x >> 1 == 7); }) == "1 == 7");
  CHECK(expansion_of([] { const double d = 0.5; CHECK(static_cast<int>(d) == 1); }) == "0 == 1");
}

TEST_CASE("framework: operands are evaluated exactly once, in every outcome") {
  int left = 0;
  int right = 0;
  int unary = 0;
  const auto lhs = [&left] { return ++left; };
  const auto rhs = [&right] { return ++right; };
  const auto flag = [&unary] {
    ++unary;
    return false;
  };
  const IsolatedResult result = run_isolated([&] {
    CHECK(lhs() == rhs());        // 1 == 1 passes
    CHECK(lhs() != rhs());        // 2 != 2 fails
    CHECK_FALSE(lhs() == rhs());  // 3 == 3 fails
    CHECK(flag());                // fails
    CHECK_FALSE(flag());          // passes
    CHECK_EQ(lhs(), rhs());       // 4 == 4 passes
    CHECK_MESSAGE(lhs() < rhs(), "five");  // fails
    REQUIRE(lhs() == rhs());      // 6 == 6 passes
  });
  CHECK(left == 6);
  CHECK(right == 6);
  CHECK(unary == 2);
  CHECK(result.passed == 4);
  CHECK(result.failed == 4);

  // The same holds when passing assertions are printed as well.
  left = right = 0;
  const IsolatedResult verbose = run_isolated([&] { CHECK(lhs() == rhs()); }, IsolatedOptions{.success = true});
  CHECK(left == 1);
  CHECK(right == 1);
  CHECK(contains(verbose.output, "PASSED: CHECK( lhs() == rhs() )\n  with expansion: 1 == 1\n"));
}

// ---- control flow --------------------------------------------------------------

TEST_CASE("framework: CHECK continues, REQUIRE abandons the test case") {
  int reached = 0;
  const IsolatedResult result = run_isolated([&] {
    CHECK(1 == 2);
    ++reached;
    CHECK_FALSE(true);
    ++reached;
    REQUIRE(1 == 1);
    ++reached;
    REQUIRE(1 == 2);
    ++reached;  // not reached
    CHECK(false);
  });
  CHECK(reached == 3);
  CHECK(result.failed == 3);
  CHECK(result.passed == 1);
  CHECK(result.exceptions == 0);
  CHECK(result.paths == 1);
  CHECK_FALSE(result.ok());
  CHECK(contains(result.output, "FAILED: REQUIRE( 1 == 2 )\n  with expansion: 1 == 2\n"));

  reached = 0;
  const IsolatedResult negated = run_isolated([&] {
    REQUIRE_FALSE(1 == 2);
    ++reached;
    REQUIRE_FALSE(1 == 1);
    ++reached;
  });
  CHECK(reached == 1);
  CHECK(negated.failed == 1);
  CHECK(contains(negated.output, "FAILED: REQUIRE_FALSE( 1 == 1 )\n  with expansion: 1 == 1\n"));
}

TEST_CASE("framework: assertions are single statements") {
  int reached = 0;
  const IsolatedResult result = run_isolated([&] {
    const bool yes = true;
    const bool no = false;
    if (no) CHECK(false);
    if (yes)
      CHECK(true);
    else
      CHECK(false);
    for (int i = 0; i < 3; ++i) CHECK(i < 3);
    if (no) FAIL("never");
    if (no) REQUIRE(false);
    if (yes) MESSAGE("note ", 1);
    ++reached;
  });
  CHECK(reached == 1);
  CHECK(result.passed == 4);
  CHECK(result.failed == 0);
  CHECK(contains(result.output, "MESSAGE: note 1\n"));
}

TEST_CASE("framework: FAIL, FAIL_CHECK, MESSAGE and CVT_TEST_FAIL_AT") {
  int reached = 0;
  const IsolatedResult result = run_isolated([&] {
    MESSAGE("just saying ", 42, '!');
    FAIL_CHECK("soft ", std::string("failure"));
    ++reached;
    CVT_TEST_FAIL_AT("/abs/root/core/tests/other_test.cpp", 12, std::string("reported elsewhere"));
    ++reached;
    if (reached == 2) FAIL("hard failure ", 1.5);
    ++reached;  // not reached
  });
  CHECK(reached == 2);
  CHECK(result.failed == 3);
  CHECK(result.passed == 0);
  CHECK(contains(result.output, ": MESSAGE: just saying 42!\n"));
  CHECK(contains(result.output, ": FAILED: soft failure\n"));
  CHECK(contains(result.output, "\ncore/tests/other_test.cpp:12: FAILED: reported elsewhere\n"));
  CHECK(contains(result.output, ": FAILED: hard failure 1.5\n"));

  // A message does not make a test case fail.
  const IsolatedResult quiet = run_isolated([] { MESSAGE("fine"); });
  CHECK(quiet.ok());
  CHECK(quiet.failed == 0);
}

TEST_CASE("framework: SKIP stops the test case without failing it") {
  int reached = 0;
  const IsolatedResult result = run_isolated([&] {
    CHECK(true);
    if (reached == 0) SKIP("needs a GPU, found ", 0);
    ++reached;  // not reached
  });
  CHECK(reached == 0);
  CHECK(result.skipped);
  CHECK(result.ok());
  CHECK(result.passed == 1);
  CHECK(contains(result.output, ": SKIPPED: needs a GPU, found 0\n"));

  // A failure before the SKIP still counts.
  const IsolatedResult failed_first = run_isolated([] {
    CHECK(false);
    SKIP("too late");
  });
  CHECK(contains(failed_first.output, ": SKIPPED: too late\n"));
  CHECK_FALSE(failed_first.skipped);
  CHECK(failed_first.failed == 1);
}

TEST_CASE("framework: unexpected exceptions are caught and reported") {
  int reached = 0;
  const IsolatedResult result = run_isolated([&] {
    CHECK(true);
    INFO("while parsing chunk ", 3);
    if (reached == 0) throw std::runtime_error("the parser blew up");
    ++reached;  // not reached
  });
  CHECK(reached == 0);
  CHECK(result.exceptions == 1);
  CHECK(result.failed == 0);
  CHECK(result.passed == 1);
  CHECK_FALSE(result.ok());
  CHECK(contains(result.output, ": ERROR: unexpected exception: the parser blew up\n"));
  CHECK(contains(result.output, "  with context:\n    while parsing chunk 3\n"));

  const IsolatedResult unknown = run_isolated([] { throw 42; });
  CHECK(unknown.exceptions == 1);
  CHECK(contains(unknown.output, ": ERROR: unexpected exception: (not derived from std::exception)\n"));

  // The run goes on after an isolated failure of any kind.
  CHECK(run_isolated([] { CHECK(true); }).ok());
}

TEST_CASE("framework: --abort-after stops at the given number of failures") {
  int reached = 0;
  const IsolatedResult result = run_isolated(
      [&] {
        for (int i = 0; i < 10; ++i) {
          CHECK(i < 0);
          ++reached;
        }
      },
      IsolatedOptions{.abort_after = 3});
  CHECK(result.failed == 3);
  CHECK(reached == 2);
}

// ---- subcases -------------------------------------------------------------------

TEST_CASE("framework: subcases run depth first, each leaf once") {
  std::string trace;
  const IsolatedResult result = run_isolated([&] {
    trace += '[';
    SUBCASE("a") {
      trace += 'a';
      SUBCASE("a1") { trace += '1'; }
      SUBCASE("a2") { trace += '2'; }
      trace += '.';
    }
    SUBCASE("b") { trace += 'b'; }
    SUBCASE("c") {
      trace += 'c';
      SUBCASE("c1") {
        trace += '1';
        SUBCASE("c11") { trace += 'x'; }
        SUBCASE("c12") { trace += 'y'; }
      }
    }
    trace += ']';
    CHECK(true);
  });
  CHECK(trace == "[a1.][a2.][b][c1x][c1y]");
  CHECK(result.paths == 5);
  CHECK(result.passed == 5);
  CHECK(result.ok());

  // No subcases: one path. Subcases with the same name are distinct.
  trace.clear();
  CHECK(run_isolated([&] { trace += 'x'; }).paths == 1);
  const IsolatedResult twins = run_isolated([&] {
    SUBCASE("same") { trace += '1'; }
    SUBCASE("same") { trace += '2'; }
  });
  CHECK(trace == "x12");
  CHECK(twins.paths == 2);
}

TEST_CASE("framework: a subcase inside a loop is entered on every iteration of its pass") {
  std::string trace;
  const IsolatedResult result = run_isolated([&] {
    trace += '[';
    for (int i = 0; i < 3; ++i) {
      SUBCASE("x") { trace += 'x'; }
    }
    SUBCASE("y") { trace += 'y'; }
    trace += ']';
  });
  CHECK(trace == "[xxx][y]");
  CHECK(result.paths == 2);
}

TEST_CASE("framework: a failed REQUIRE abandons one path, the other subcases still run") {
  std::string trace;
  const IsolatedResult first = run_isolated([&] {
    trace += '[';
    SUBCASE("a") {
      trace += 'a';
      REQUIRE(false);
    }
    SUBCASE("b") { trace += 'b'; }
    SUBCASE("c") { trace += 'c'; }
    trace += ']';
    CHECK(true);
  });
  CHECK(trace == "[a[b][c]");
  CHECK(first.paths == 3);
  CHECK(first.failed == 1);
  CHECK(first.passed == 2);

  trace.clear();
  const IsolatedResult nested = run_isolated([&] {
    trace += '[';
    SUBCASE("p") {
      trace += 'p';
      SUBCASE("x") {
        trace += 'x';
        REQUIRE(false);
      }
      SUBCASE("y") { trace += 'y'; }
      trace += '.';
    }
    SUBCASE("q") { trace += 'q'; }
    trace += ']';
  });
  CHECK(trace == "[px[py.][q]");
  CHECK(nested.paths == 3);
  CHECK(nested.failed == 1);

  // An exception is handled the same way.
  trace.clear();
  const IsolatedResult thrown = run_isolated([&] {
    SUBCASE("a") {
      trace += 'a';
      throw std::runtime_error("boom");
    }
    SUBCASE("b") { trace += 'b'; }
  });
  CHECK(trace == "ab");
  CHECK(thrown.exceptions == 1);
  CHECK(thrown.paths == 2);
}

TEST_CASE("framework: the pass that looks for subcases after an abandoned last one is discarded") {
  // When the abandoned subcase is the last one, the body is entered once more
  // to find that out. That pass enters no subcase; whatever it reports does
  // not count.
  std::string trace;
  const IsolatedResult result = run_isolated([&] {
    bool prepared = false;
    trace += '[';
    SUBCASE("a") {
      prepared = true;
      trace += 'a';
    }
    SUBCASE("b") {
      prepared = true;
      trace += 'b';
      REQUIRE(1 == 2);
    }
    trace += ']';
    CHECK(prepared);  // fails in the extra pass only
    MESSAGE("after the subcases");
  });
  CHECK(trace == "[a][b[]");
  CHECK(result.paths == 2);
  CHECK(result.failed == 1);
  CHECK(result.passed == 1);
  CHECK(occurrences(result.output, "FAILED") == 1);
  CHECK(occurrences(result.output, "MESSAGE") == 1);

  // Nothing is entered again when the body fails outside every subcase.
  trace.clear();
  const IsolatedResult outside = run_isolated([&] {
    trace += '[';
    REQUIRE(false);
    SUBCASE("never") { trace += 'n'; }
  });
  CHECK(trace == "[");
  CHECK(outside.paths == 1);
  CHECK(outside.failed == 1);
}

TEST_CASE("framework: failures name the test case and the subcase path once per path") {
  const IsolatedResult result = run_isolated(
      [] {
        CHECK(1 == 2);
        SUBCASE("outer") {
          SUBCASE("inner one") {
            CHECK(3 == 4);
            CHECK(5 == 6);
          }
          SUBCASE("inner two") { CHECK(true); }
        }
        CHECK(7 == 8);
      },
      {}, "the name of the test");
  CHECK(result.paths == 2);
  CHECK(result.failed == 6);
  // In both paths: once for the failure before the subcases, and once more
  // when the path has become longer.
  CHECK(occurrences(result.output,
                    "TEST CASE: the name of the test  [core/tests/support/test_framework_test.cpp:") == 4);
  CHECK(occurrences(result.output, "  SUBCASE: outer\n    SUBCASE: inner one\n") == 1);
  CHECK(occurrences(result.output, "  SUBCASE: outer\n    SUBCASE: inner two\n") == 1);
  CHECK(occurrences(result.output, "FAILED: CHECK( 7 == 8 )") == 2);

  const IsolatedResult plain = run_isolated([] {
    CHECK(1 == 2);
    CHECK(3 == 4);
  });
  CHECK(occurrences(plain.output, "TEST CASE: ") == 1);
}

// ---- context and messages ----------------------------------------------------------

TEST_CASE("framework: INFO and CAPTURE apply to failures in their scope only") {
  const IsolatedResult result = run_isolated([] {
    const std::string format = "png";
    CHECK(1 == 0);  // no context yet
    INFO("outer ", 1, ' ', format);
    {
      const int width = 640;
      CAPTURE(width);
      CAPTURE(format);
      CHECK(2 == 0);
    }
    CHECK(3 == 0);
  });
  CHECK(result.failed == 3);
  const std::size_t first = result.output.find("CHECK( 1 == 0 )");
  const std::size_t second = result.output.find("CHECK( 2 == 0 )");
  const std::size_t third = result.output.find("CHECK( 3 == 0 )");
  REQUIRE(first < second);
  REQUIRE(second < third);
  REQUIRE(third != std::string::npos);
  const std::string_view output = result.output;
  CHECK_FALSE(contains(output.substr(first, second - first), "with context"));
  CHECK(contains(output.substr(second, third - second),
                 "  with context:\n    outer 1 png\n    width := 640\n    format := \"png\"\n"));
  CHECK(contains(output.substr(third), "  with context:\n    outer 1 png\n"));
  CHECK_FALSE(contains(output.substr(third), "width"));
}

TEST_CASE("framework: INFO, CAPTURE and assertion messages are lazy") {
  int evaluations = 0;
  const auto counted = [&evaluations] {
    ++evaluations;
    return 7;
  };
  const IsolatedResult passing = run_isolated([&] {
    for (int i = 0; i < 100; ++i) {
      INFO("value ", counted());
      CAPTURE(counted());
      CHECK(i >= 0);
      CHECK_MESSAGE(i >= 0, "message ", counted());
      REQUIRE_MESSAGE(i >= 0, counted());
    }
  });
  CHECK(passing.passed == 300);
  CHECK(evaluations == 0);

  const IsolatedResult failing = run_isolated([&] {
    INFO("value ", counted());
    CHECK_MESSAGE(1 == 2, "message ", counted(), " and ", std::string("text"), ' ', 1.5);
    CHECK(3 == 4);
  });
  CHECK(failing.failed == 2);
  CHECK(evaluations == 3);  // the INFO once per failure, the message once
  CHECK(contains(failing.output, "  with expansion: 1 == 2\n"
                                 "  message: message 7 and text 1.5\n"
                                 "  with context:\n"
                                 "    value 7\n"));

  // The values are read when the failure is reported, not when INFO was written.
  const IsolatedResult late = run_isolated([] {
    int step = 1;
    INFO("step ", step);
    step = 2;
    CHECK(false);
  });
  CHECK(contains(late.output, "    step 2\n"));

  const IsolatedResult fatal = run_isolated([] {
    const std::string name = "a.png";
    REQUIRE_MESSAGE(name.empty(), "file: ", name);
  });
  CHECK(contains(fatal.output, "FAILED: REQUIRE( name.empty() )\n  message: file: a.png\n"));
}

// ---- long values --------------------------------------------------------------------

TEST_CASE("framework: long strings are cut in the middle and located by their first difference") {
  std::string text(1000, 'a');
  CHECK(stringify(text) ==
        '"' + std::string(200, 'a') + "\"...<600 bytes omitted>...\"" + std::string(200, 'a') + "\" (length 1000)");
  CHECK(stringify(std::string(440, 'b')) == '"' + std::string(440, 'b') + '"');

  const IsolatedResult result = run_isolated([&] {
    std::string other = text;
    other[700] = 'b';
    CHECK(text == other);
  });
  CHECK(contains(result.output, "  with expansion:\n    \"aaaa"));
  CHECK(contains(result.output, "\n    ==\n    \"aaaa"));
  CHECK(contains(result.output, "\n  first difference at index 700; lengths 1000 and 1000\n"
                                "    lhs: ...\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"...\n"
                                "    rhs: ...\"aaaaaaaaaaaaaaaaaaaaaaaabaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"...\n"
                                "                                     ^\n"));

  const IsolatedResult lines = run_isolated([] {
    const std::string a = "line one\nline two is here\nline three is the long one";
    const std::string b = "line one\nline two is here\nline thrEe is the long one";
    CHECK(a == b);
  });
  CHECK(contains(lines.output, "\n  first difference at index 34 (line 3, column 9); lengths 52 and 52\n"
                               "    lhs: ...\"ine two is here\\nline three is the long one\"\n"
                               "    rhs: ...\"ine two is here\\nline thrEe is the long one\"\n"
                               "                                      ^\n"));

  // One string is a prefix of the other.
  const IsolatedResult prefix = run_isolated([] { CHECK(std::string(60, 'x') == std::string(50, 'x')); });
  CHECK(contains(prefix.output, "\n  first difference at index 50; lengths 60 and 50\n"));

  // Short strings are fully visible: no note.
  const IsolatedResult brief = run_isolated([] { CHECK(std::string("abc") == "abd"); });
  CHECK_FALSE(contains(brief.output, "first difference"));
  // Equal strings failing a != have no difference to show.
  const IsolatedResult equal = run_isolated([] { CHECK(std::string(100, 'x') != std::string(100, 'x')); });
  CHECK(equal.failed == 1);
  CHECK_FALSE(contains(equal.output, "first difference"));
}

TEST_CASE("framework: long byte runs show a window around their first difference") {
  const IsolatedResult result = run_isolated([] {
    const std::vector<std::byte> a(300, std::byte{0x11});
    std::vector<std::byte> b = a;
    b[200] = std::byte{0x22};
    CHECK(a == b);
    b.resize(100);
    CHECK(std::span<const std::byte>(a).size() == b.size());
    CHECK(a == b);
  });
  CHECK(result.failed == 3);
  CHECK(contains(result.output, "  first difference at index 200 (0xc8); sizes 300 and 300\n"
                                "    lhs[192...]: 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11\n"
                                "    rhs[192...]: 11 11 11 11 11 11 11 11 22 11 11 11 11 11 11 11\n"
                                "                                         ^^\n"));
  CHECK(contains(result.output, "  first difference at index 100 (0x64); sizes 300 and 100\n"
                                "    lhs[92...]: 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11\n"
                                "    rhs[92...]: 11 11 11 11 11 11 11 11 (ends)\n"));
}

// ---- the extra assertion macros -------------------------------------------------------

TEST_CASE("framework: comparison macros") {
  const IsolatedResult result = run_isolated([] {
    const int three = 3;
    CHECK_EQ(three, 3);
    CHECK_NE(three, 4);
    CHECK_LT(three, 4);
    CHECK_LE(three, 3);
    CHECK_GT(three, 2);
    CHECK_GE(three, 3);
    REQUIRE_EQ(std::string("a"), "a");
    REQUIRE_NE(three, 0);
    REQUIRE_LT(2, three);
    REQUIRE_LE(three, three);
    REQUIRE_GT(three, 2);
    REQUIRE_GE(three, three);
    CHECK_EQ(three, 4);
    CHECK_NE(three, 3);
    CHECK_LT(three, 3);
    CHECK_LE(three, 2);
    CHECK_GT(three, 3);
    CHECK_GE(three, 4);
    REQUIRE_EQ(three, 5);
    CHECK(false);  // not reached
  });
  CHECK(result.passed == 12);
  CHECK(result.failed == 7);
  CHECK(contains(result.output, "FAILED: CHECK_EQ( three, 4 )\n  with expansion: 3 == 4\n"));
  CHECK(contains(result.output, "FAILED: CHECK_GE( three, 4 )\n  with expansion: 3 >= 4\n"));
  CHECK(contains(result.output, "FAILED: REQUIRE_EQ( three, 5 )\n  with expansion: 3 == 5\n"));
}

TEST_CASE("framework: approximate comparison") {
  const double not_a_number = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const IsolatedResult result = run_isolated([&] {
    CHECK_APPROX(1.0, 1.05, 0.1);
    CHECK_APPROX(3, 3.5, 0.5);
    CHECK_APPROX(infinity, infinity, 0.0);
    REQUIRE_APPROX(-1.0f, -1.0, 0.0);
    CHECK(0.1 + 0.2 == approx(0.3));
    CHECK(100.0 == approx(104.0).rel(0.05));
    CHECK(0.1 + 0.2 != approx(0.4));
    CHECK(not_a_number != approx(not_a_number));
    CHECK(1.0f == approx(1.0));
    // six failures
    CHECK_APPROX(1.0, 1.5, 0.25);
    CHECK_APPROX(not_a_number, not_a_number, 1.0);
    CHECK_APPROX(infinity, -infinity, 1e300);
    CHECK_APPROX(1.0, infinity, 1e300);
    CHECK(not_a_number == approx(not_a_number).abs(1.0));
    CHECK(3 == approx(3.5).abs(0.1));
  });
  CHECK(result.passed == 9);
  CHECK(result.failed == 6);
  CHECK(contains(result.output, "FAILED: CHECK_APPROX( 1.0, 1.5, 0.25 )\n"
                                "  with expansion: 1.0 !~= 1.5 (difference 0.5, tolerance 0.25)\n"));
  CHECK(contains(result.output, "  with expansion: nan !~= nan (difference nan, tolerance 1.0)\n"));
  CHECK(contains(result.output, "  with expansion: 3 == approx(3.5, abs 0.1, rel 0.0)\n"));
  CHECK(stringify(approx(2.0)) == "approx(2.0, abs 1e-12, rel 1e-09)");
  CHECK(stringify(approx(2.0).rel(0.5)) == "approx(2.0, abs 0.0, rel 0.5)");
}

TEST_CASE("framework: CHECK_OK and CHECK_STATUS") {
  int reached = 0;
  const IsolatedResult result = run_isolated([&] {
    const Result<int> good = 7;
    const Result<int> bad = cvt::fail(Status::Decode, "truncated header");
    const Result<void> nothing;
    CHECK_OK(good);
    CHECK_OK(nothing);
    REQUIRE_OK(Result<std::string>("temporary"));
    CHECK_STATUS(bad, Status::Decode);
    REQUIRE_STATUS(bad, Status::Decode);
    ++reached;
    CHECK_OK(bad);
    CHECK_STATUS(bad, Status::Io);
    CHECK_STATUS(good, Status::Decode);
    ++reached;
    REQUIRE_OK(bad);
    ++reached;  // not reached
  });
  CHECK(reached == 2);
  CHECK(result.passed == 5);
  CHECK(result.failed == 4);
  CHECK(contains(result.output, "FAILED: CHECK_OK( bad )\n"
                                "  with expansion: error{decode error, \"truncated header\"}\n"));
  CHECK(contains(result.output, "FAILED: CHECK_STATUS( bad, Status::Io )\n"
                                "  with expansion: error{decode error, \"truncated header\"} "
                                "(expected status: I/O error)\n"));
  CHECK(contains(result.output, "FAILED: CHECK_STATUS( good, Status::Decode )\n"
                                "  with expansion: holds a value (expected status: decode error)\n"));
  CHECK(contains(result.output, "FAILED: REQUIRE_OK( bad )\n"));
}

// ---- the runner ------------------------------------------------------------------------

TEST_CASE("framework: --success prints passing assertions") {
  const IsolatedResult result = run_isolated(
      [] {
        const int a = 1;
        CHECK(a == 1);
        CHECK(true);
        CHECK(a == 2);
      },
      IsolatedOptions{.success = true});
  CHECK(result.passed == 2);
  CHECK(result.failed == 1);
  CHECK(contains(result.output, ": PASSED: CHECK( a == 1 )\n  with expansion: 1 == 1\n"));
  CHECK(contains(result.output, ": PASSED: CHECK( true )\n"));
  CHECK(contains(result.output, ": FAILED: CHECK( a == 2 )\n  with expansion: 1 == 2\n"));

  // Without it a passing run prints nothing at all.
  CHECK(run_isolated([] { CHECK(true); }).output.empty());
}

TEST_CASE("framework: wildcard patterns") {
  CHECK(fw::wildcard_match("", ""));
  CHECK(fw::wildcard_match("*", ""));
  CHECK(fw::wildcard_match("*", "anything at all"));
  CHECK(fw::wildcard_match("zip: *", "zip: reading"));
  CHECK(fw::wildcard_match("*reading", "zip: reading"));
  CHECK(fw::wildcard_match("*: read*", "zip: reading"));
  CHECK(fw::wildcard_match("z?p*", "zip: reading"));
  CHECK(fw::wildcard_match("a*b*c", "a-b-b-c"));
  CHECK(fw::wildcard_match("a**c", "abc"));
  CHECK(fw::wildcard_match("???", "abc"));
  CHECK(fw::wildcard_match("exact name", "exact name"));
  CHECK_FALSE(fw::wildcard_match("", "x"));
  CHECK_FALSE(fw::wildcard_match("zip", "zip: reading"));  // the whole name must match
  CHECK_FALSE(fw::wildcard_match("zip: *", "gzip: reading"));
  CHECK_FALSE(fw::wildcard_match("??", "abc"));
  CHECK_FALSE(fw::wildcard_match("a*b*c", "a-b-b-d"));
  CHECK_FALSE(fw::wildcard_match("ZIP*", "zip: reading"));  // case sensitive
  CHECK_FALSE(fw::wildcard_match("a?", "a"));
}

TEST_CASE("framework: test-case and source-file filters") {
  using fw::Selection;
  const std::string_view file = "/abs/root/core/tests/util/zip_test.cpp";
  const auto selects = [&](std::initializer_list<std::string_view> arguments, std::string_view name) {
    return fw::selects(arguments, name, file);
  };
  CHECK(selects({}, "zip: reading") == Selection::Selected);
  CHECK(selects({"--test-case=zip: *"}, "zip: reading") == Selection::Selected);
  CHECK(selects({"-tc=zip: *"}, "zip: reading") == Selection::Selected);
  CHECK(selects({"-tc=zip: *"}, "text: split") == Selection::NotSelected);
  // Comma lists, also given several times.
  CHECK(selects({"-tc=json: *,zip: *"}, "zip: reading") == Selection::Selected);
  CHECK(selects({"-tc=json: *", "-tc=zip: *"}, "zip: reading") == Selection::Selected);
  CHECK(selects({"-tc=json: *,,xml: *"}, "zip: reading") == Selection::NotSelected);
  // A comma in a name is written with a backslash.
  CHECK(selects({"-tc=zip: names\\, encryption and *"}, "zip: names, encryption and methods") == Selection::Selected);
  CHECK(selects({"-tc=zip: names, encryption and *"}, "zip: names, encryption and methods") == Selection::NotSelected);
  CHECK(selects({"-tc=zip: names, encryption and *"}, "zip: names") == Selection::Selected);
  CHECK(fw::filter_matches("a\\,b,c", "a,b"));
  CHECK(fw::filter_matches("a\\,b,c", "c"));
  CHECK_FALSE(fw::filter_matches("a\\,b,c", "a"));
  CHECK(fw::filter_matches("back\\slash", "back\\slash"));
  CHECK_FALSE(fw::filter_matches("", "anything"));
  // Excludes win over includes.
  CHECK(selects({"-tce=zip: *"}, "zip: reading") == Selection::NotSelected);
  CHECK(selects({"-tce=zip: *"}, "text: split") == Selection::Selected);
  CHECK(selects({"-tc=zip: *", "--test-case-exclude=*reading"}, "zip: reading") == Selection::NotSelected);
  CHECK(selects({"-tc=zip: *", "--test-case-exclude=*reading"}, "zip: writing") == Selection::Selected);
  // Exact names take no wildcards and no comma splitting.
  CHECK(selects({"--test-case-exact=zip: names, encryption *"}, "zip: names, encryption *") == Selection::Selected);
  CHECK(selects({"--test-case-exact=zip: *"}, "zip: reading") == Selection::NotSelected);
  // Source files match on the printed path or on the file name.
  CHECK(selects({"-sf=core/tests/util/zip_test.cpp"}, "x") == Selection::Selected);
  CHECK(selects({"--source-file=*/util/*"}, "x") == Selection::Selected);
  CHECK(selects({"-sf=zip_test.cpp"}, "x") == Selection::Selected);
  CHECK(selects({"-sf=zip_*"}, "x") == Selection::Selected);
  CHECK(selects({"-sf=*json*"}, "x") == Selection::NotSelected);
  CHECK(selects({"-sfe=*/util/*"}, "x") == Selection::NotSelected);
  CHECK(selects({"-sf=*zip*", "-tc=y"}, "x") == Selection::NotSelected);
  // Everything else on the command line.
  CHECK(selects({"--success", "-s", "--no-colors", "-nc", "--colors", "--durations", "-d", "--absolute-paths",
                 "--abort-after=3", "-aa=1", "--order-by=rand", "-ob=name", "--order-by=file", "--rand-seed=42",
                 "-rs=7", "--list-test-cases", "-ltc", "--count", "-c", "--no-summary", "--help", "-h",
                 "--success=false", "--no-colors=true"},
                "x") == Selection::Selected);
  CHECK(selects({"--no-such-option"}, "x") == Selection::UsageError);
  CHECK(selects({"zip: reading"}, "x") == Selection::UsageError);
  CHECK(selects({"--test-case"}, "x") == Selection::UsageError);
  CHECK(selects({"--abort-after=many"}, "x") == Selection::UsageError);
  CHECK(selects({"--abort-after"}, "x") == Selection::UsageError);
  CHECK(selects({"--order-by=size"}, "x") == Selection::UsageError);
  CHECK(selects({"--success=maybe"}, "x") == Selection::UsageError);
}

TEST_CASE("framework: reported paths are relative to the repository") {
  CHECK(fw::display_path("/abs/root/convertible/core/tests/util/zip_test.cpp") == "core/tests/util/zip_test.cpp");
  CHECK(fw::display_path("Z:\\abs\\root\\core\\tests\\util\\zip_test.cpp") == "core/tests/util/zip_test.cpp");
  CHECK(fw::display_path("/a/core/b/core/tests/x_test.cpp") == "core/tests/x_test.cpp");
  CHECK(fw::display_path("core/tests/x_test.cpp") == "core/tests/x_test.cpp");
  CHECK(fw::display_path("elsewhere/x_test.cpp") == "elsewhere/x_test.cpp");

  const IsolatedResult relative = run_isolated([] { CHECK(false); });
  CHECK(relative.output.starts_with("TEST CASE: isolated  [core/tests/support/test_framework_test.cpp:"));
  CHECK(contains(relative.output, "\ncore/tests/support/test_framework_test.cpp:"));
  CHECK(fw::display_path(__FILE__) == "core/tests/support/test_framework_test.cpp");

  const IsolatedResult absolute = run_isolated([] { CHECK(false); }, IsolatedOptions{.absolute_paths = true});
  CHECK(contains(absolute.output, std::string("\n") + __FILE__ + ":"));
}

#if defined(CVT_TEST_DATA_DIR)
TEST_CASE("framework: a helper reports its failure at the line of its caller") {
  int line = 0;
  const IsolatedResult result = run_isolated([&line] {
    line = __LINE__ + 1;
    const cvt::Bytes data = cvt::test::fixture("support/not-there.bin");
    CHECK(data.empty());
    CHECK_FALSE(cvt::test::has_fixture("../outside"));
  });
  CHECK(result.failed == 2);
  CHECK(result.passed == 2);
  CHECK(contains(result.output, "\ncore/tests/support/test_framework_test.cpp:" + std::to_string(line) +
                                    ": FAILED: fixture 'support/not-there.bin' not found below core/tests/data/"));
  CHECK(contains(result.output, "test_framework_test.cpp:" + std::to_string(line + 2) +
                                    ": FAILED: invalid fixture name '../outside'"));
}
#endif

// ---- isolation and threads ---------------------------------------------------------------

TEST_CASE("framework: an isolated run does not leak into the surrounding one") {
  const std::uint64_t before = fw::hot.passed.load();
  INFO("context of the surrounding test case");
  const IsolatedResult result = run_isolated([] {
    for (int i = 0; i < 50; ++i) CHECK(true);
    CHECK(false);
    // Runs nest.
    const IsolatedResult inner = run_isolated([] {
      CHECK(true);
      CHECK(false);
      CHECK(false);
    });
    CHECK(inner.passed == 1);
    CHECK(inner.failed == 2);
  });
  CHECK(fw::hot.passed.load() == before);
  CHECK(result.passed == 52);
  CHECK(result.failed == 1);
  CHECK(occurrences(result.output, "FAILED") == 1);
  CHECK_FALSE(contains(result.output, "context of the surrounding"));
}

TEST_CASE("framework: CHECK may be used from threads started by a test") {
  constexpr int kThreads = 4;
  constexpr int kChecks = 2000;
  const IsolatedResult result = run_isolated([] {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([t] {
        INFO("worker ", t);
        for (int i = 0; i < kChecks; ++i) {
          CHECK(i >= 0);
          if (i % 500 == 0) CHECK(i < 0);
        }
      });
    }
    for (std::thread& thread : threads) thread.join();
  });
  CHECK(result.passed == kThreads * kChecks);
  CHECK(result.failed == kThreads * 4);
  CHECK(occurrences(result.output, "FAILED: CHECK( i < 0 )") == kThreads * 4);
  // Context belongs to the thread that set it.
  for (int t = 0; t < kThreads; ++t) {
    CHECK(occurrences(result.output, "    worker " + std::to_string(t) + "\n") == 4);
  }
}

