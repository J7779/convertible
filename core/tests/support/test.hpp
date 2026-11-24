// The in-house unit-test framework of the Convertible core.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// ============================================================================
// Writing tests
// ============================================================================
//
// Include "support/test.hpp" (or "support/test_support.hpp", which adds the
// engine helpers) and write test cases as free functions:
//
//   TEST_CASE("hex: encodes lower case") {
//     const Bytes data = test::bytes("\x01\xab");
//     CHECK(hex_encode(data) == "01ab");
//   }
//
// Test cases register themselves; every *.cpp below core/tests/ is compiled
// into the one `convertible_tests` executable. Names are free text and by
// convention start with "<area>: ". They need not be unique.
//
// Assertions
// ----------
//   CHECK(expr)            records a failure and continues
//   REQUIRE(expr)          records a failure and abandons the current path
//                          through the test case (use it when what follows
//                          would crash or be meaningless)
//   CHECK_FALSE(expr), REQUIRE_FALSE(expr)       pass when expr is false
//   CHECK_MESSAGE(expr, parts...), REQUIRE_MESSAGE(expr, parts...),
//   CHECK_FALSE_MESSAGE(expr, parts...), REQUIRE_FALSE_MESSAGE(expr, parts...)
//                          as above with a message; the parts are only
//                          evaluated and formatted when the assertion fails
//   CHECK_EQ/NE/LT/LE/GT/GE(a, b), REQUIRE_EQ/...(a, b)
//                          explicit comparisons, for operands that cannot be
//                          written inside CHECK (see "Limits" below)
//   CHECK_APPROX(a, b, tolerance), REQUIRE_APPROX(...)
//                          |a - b| <= tolerance (absolute); NaN never passes
//   CHECK(x == approx(1.5).abs(1e-9)), approx(v).rel(1e-6), approx(v)
//                          cvt::test::approx: the tolerance travels with the
//                          expected value; with neither abs() nor rel()
//                          the tolerance is max(1e-12, 1e-9 * magnitude); with
//                          one of them given the other one is 0
//   CHECK_OK(result), REQUIRE_OK(result)
//                          a cvt::Result<T> holds a value; a failure prints
//                          the error code and message
//   CHECK_STATUS(result, Status::Decode), REQUIRE_STATUS(...)
//                          the result holds an error with exactly that code
//   FAIL(parts...)         fails and abandons the path
//   FAIL_CHECK(parts...)   fails and continues
//   SKIP(parts...)         stops the test case and reports it as skipped
//   MESSAGE(parts...)      prints a line, does not fail
//   CVT_TEST_FAIL_AT(file, line, message)
//                          non-fatal failure attributed to another location,
//                          for helpers that take a std::source_location
//
// Each assertion is one statement; `if (!result) CHECK(x);` is fine.
//
// A failing comparison shows both operands:
//
//   core/tests/util/hex_test.cpp:42: FAILED: CHECK( hex_encode(data) == "01ab" )
//     with expansion: "01AB" == "01ab"
//
// The operands of `== != < <= > >=` are evaluated exactly once and are never
// copied. `&&` and `||` keep their short-circuit behaviour and need no extra
// parentheses, but only the comparison (or value) on their left-hand side is
// decomposed, and the expansion is not printed for them: prefer two CHECKs.
//
// Context
// -------
//   INFO(parts...)   attaches a note to every failure reported until the end
//                    of the enclosing scope
//   CAPTURE(x)       the same, formatted as `x := <value>`
// Both are lazy: the parts are captured by reference and only formatted when
// a failure is reported, so they cost almost nothing in loops. Message parts
// are concatenated without separators; strings are inserted as they are,
// everything else is printed like an operand.
//
// Subcases
// --------
//   TEST_CASE("zip: hostile archives") {
//     Bytes archive = make_archive();          // runs once per path
//     SUBCASE("encrypted entry") { ... }
//     SUBCASE("bad checksum") { ... SUBCASE("in the last entry") { ... } }
//     CHECK(still_consistent(archive));        // runs once per path
//   }
// The body is run once for every leaf subcase, depth first in source order,
// each time entering only the subcases on the way to that leaf. After a
// REQUIRE failure or an exception inside a subcase the remaining subcases are
// still run. To find the ones that follow the abandoned subcase the body is
// entered again; if none follows, that extra pass executes the code outside
// the subcases without entering any and its results are discarded - so code
// after the subcases must not crash when none of them ran (guard it with
// REQUIRE, which is discarded as well).
//
// How values are printed
// ----------------------
// bool, integers, floating point (shortest text that round-trips; `f`/`L`
// suffix for float/long double), `char` as 'c', std::byte as 0xNN, strings
// quoted with escapes (invalid UTF-8 as \xNN, long strings cut in the middle,
// and two long unequal strings get the index of the first difference with an
// excerpt), pointers, enums (with the text of operator<< when there is one),
// std::optional, pair/tuple, std::expected (the value or `error{...}`),
// variants, ranges (first 16 elements and the size; std::byte ranges as a hex
// dump; unequal ranges get the first differing index), paths, durations.
// For your own type, in order of preference:
//
//   std::ostream& operator<<(std::ostream&, const MyType&);   // found by ADL
//
//   namespace cvt::test {
//   template <>
//   struct Printer<MyType> {                                   // wins over <<
//     static std::string to_string(const MyType& value);
//   };
//   }
//
// Both must be declared before the first assertion that prints the type and
// must be visible in every translation unit that does. Anything else prints
// as `{? TypeName}`. `cvt::test::stringify(value)` gives the same text.
//
// Threads
// -------
// CHECK-style assertions, INFO and MESSAGE may be used from threads started
// by a test (INFO context is per thread). REQUIRE, FAIL and SKIP must only
// fail on the thread that runs the test case: on any other thread the failure
// is reported and the process is aborted. Join your threads before the test
// case returns.
//
// Limits
// ------
//  * `CHECK(a == b == c)` and assignments do not compile; `CHECK(p == 0)` for
//    a pointer p does not either: write `p == nullptr`.
//  * Operands are bound to `const&`: a type whose operator== is not const
//    cannot be compared inside CHECK.
//  * A string literal operand is handed to the comparison as a `const char*`
//    whenever the other operand can be compared with one (std::string,
//    std::string_view, ...), as if it had been written `+"literal"`.
//  * A test that catches `...` swallows the abandon-path signal of REQUIRE;
//    catch `const std::exception&` instead.
//  * No REQUIRE in destructors or noexcept functions.
//
// Running tests
// -------------
//   convertible_tests [options]
//     -tc=,  --test-case=<patterns>          run matching test cases
//     -tce=, --test-case-exclude=<patterns>  ... except these
//     -sf=,  --source-file=<patterns>        test cases from matching files
//     -sfe=, --source-file-exclude=<patterns>
//            --test-case-exact=<name>        one exact name, no wildcards
//     -ltc,  --list-test-cases               names, one per line, then a count
//     -c,    --count                         number of matching test cases
//     -s,    --success                       also print passing assertions
//     -aa=,  --abort-after=<n>               stop after n failed assertions
//     -d,    --durations                     time and number of assertions of
//                                            every test case, slowest first
//     -ob=,  --order-by=file|name|rand       default: file, then line
//     -rs=,  --rand-seed=<n>                 seed for --order-by=rand
//     -nc,   --no-colors / --colors          default: colours on a terminal
//            --absolute-paths                do not shorten file names
//            --no-summary                    no "[convertible_tests]" lines
//                                            (--no-header means the same)
//     -h,    --help
// Patterns are comma separated, case sensitive, must match the whole name and
// may use `*` (any text) and `?` (one character); write `\,` for a comma. A
// source-file pattern is matched against the path as printed
// (core/tests/util/zip_test.cpp) and against the file name alone.
// Exit code: 0 success (also when nothing matched), 1 failures, 2 bad usage.
//
// ============================================================================
// Integration notes
// ============================================================================
// This header is included by every test file. Exactly one translation unit
// must define CVT_TEST_MAIN (framework implementation + main) or
// CVT_TEST_IMPLEMENTATION (implementation only; call cvt::test::run) before
// including it. Optional compile definitions for that translation unit:
//   CVT_TEST_SOURCE_ROOT="/abs/path/to/repo"   prefix removed from file names
//       (default: everything before the last "/core/" is removed)
//   CVT_TEST_PROGRAM_NAME="convertible_tests"  name in the summary lines
#ifndef CVT_TESTS_SUPPORT_TEST_HPP
#define CVT_TESTS_SUPPORT_TEST_HPP

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <ostream>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "engine/errors.hpp"

namespace cvt::test {

/// Customisation point for printing values in failure messages. Specialise it
/// (before the first assertion that prints the type) with
///   static std::string to_string(const T& value);
template <class T>
struct Printer;

/// The text an assertion would print for `value`.
template <class T>
[[nodiscard]] std::string stringify(const T& value);

/// Entry point of the runner; `main` forwards to it. Returns the exit code.
int run(int argc, const char* const* argv);

}  // namespace cvt::test

// Everything in `fw` is the machinery behind the macros; not for tests.
namespace cvt::test::fw {

// ---------------------------------------------------------------------------
// Services implemented once, in the translation unit with the implementation
// ---------------------------------------------------------------------------

/// Where an assertion was written.
struct Site {
  const char* file;
  int line;
  const char* macro;  ///< "CHECK", "REQUIRE_FALSE", ...
  const char* text;   ///< the source text of the arguments
};

inline constexpr unsigned kFatal = 1u;   ///< REQUIRE family: abandon the path on failure
inline constexpr unsigned kNegate = 2u;  ///< *_FALSE family

/// The only state touched by a passing assertion.
struct HotState {
  std::atomic<std::uint64_t> passed{0};
  std::atomic<bool> verbose{false};  ///< --success: passing assertions are reported too
};
inline constinit HotState hot;

/// True (and counted) when a passing assertion needs no further work.
[[nodiscard]] inline bool fast_pass(bool ok) noexcept {
  if (ok && !hot.verbose.load(std::memory_order_relaxed)) [[likely]] {
    hot.passed.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

/// A reference to a callable `void(std::string&)` that appends message text;
/// only invoked when something is reported.
class LazyText {
 public:
  constexpr LazyText() noexcept = default;
  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, LazyText>)
  LazyText(const F& function) noexcept  // NOLINT(google-explicit-constructor)
      : object_(&function),
        render_([](const void* object, std::string& out) { (*static_cast<const F*>(object))(out); }) {}

  [[nodiscard]] explicit operator bool() const noexcept { return render_ != nullptr; }
  void render(std::string& out) const { render_(object_, out); }

 private:
  const void* object_ = nullptr;
  void (*render_)(const void*, std::string&) = nullptr;
};

enum class Op : unsigned char { Eq, Ne, Lt, Le, Gt, Ge };

void report_assertion(const Site& site, unsigned flags, bool passed, std::string_view expansion, std::string_view note,
                      LazyText detail);
void report_comparison(const Site& site, unsigned flags, bool passed, std::string_view lhs, Op op, std::string_view rhs,
                       std::string_view note, LazyText detail);
void report_approx(const Site& site, unsigned flags, bool passed, double lhs, double rhs, double tolerance);
[[noreturn]] void fail_fatal(const char* file, int line, std::string_view text);
void fail_soft(const char* file, int line, std::string_view text);
void message(const char* file, int line, std::string_view text);
[[noreturn]] void skip(const char* file, int line, std::string_view reason);

void append_signed(std::string& out, long long value);
void append_unsigned(std::string& out, unsigned long long value);
void append_float(std::string& out, float value);
void append_double(std::string& out, double value);
void append_long_double(std::string& out, long double value);
void append_quoted_char(std::string& out, char value);
void append_code_point(std::string& out, char32_t value);
/// `text` in double quotes with escapes; long text is cut in the middle.
void append_quoted(std::string& out, std::string_view text);
/// UTF-16 / UTF-32 text (`unit_size` 2 or 4 bytes per code unit) as a quoted UTF-8 string.
void append_quoted_wide(std::string& out, const void* data, std::size_t count, std::size_t unit_size);
void append_pointer(std::string& out, const void* pointer);
void append_hex_byte(std::string& out, unsigned value);
void append_bytes(std::string& out, const void* data, std::size_t size);
void append_duration_unit(std::string& out, long long numerator, long long denominator);
void append_type_name(std::string& out, std::string_view signature, std::string_view probe_signature, bool is_empty);
/// A note locating the first difference of two unequal strings / byte runs; "" when it would not help.
[[nodiscard]] std::string string_difference(std::string_view lhs, std::string_view rhs);
[[nodiscard]] std::string bytes_difference(const void* lhs, std::size_t lhs_size, const void* rhs,
                                           std::size_t rhs_size);

/// An std::ostream writing into a string, for types printed with operator<<.
class StreamCapture {
 public:
  StreamCapture();
  ~StreamCapture();
  StreamCapture(const StreamCapture&) = delete;
  StreamCapture& operator=(const StreamCapture&) = delete;
  [[nodiscard]] std::ostream& stream() noexcept;
  [[nodiscard]] std::string_view text() const noexcept;

 private:
  struct State;
  State* state_;
};

// ---------------------------------------------------------------------------
// Value printing
// ---------------------------------------------------------------------------

template <class T>
void put(std::string& out, const T& value);

template <class T>
[[nodiscard]] std::string_view signature_of() noexcept {
#if defined(__clang__) || defined(__GNUC__)
  return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
  return __FUNCSIG__;
#else
  return {};
#endif
}

namespace adl {
using std::begin;
using std::end;
template <class T>
auto begin_of(const T& range) -> decltype(begin(range)) {
  return begin(range);
}
template <class T>
auto end_of(const T& range) -> decltype(end(range)) {
  return end(range);
}
}  // namespace adl

template <class T>
concept HasPrinter = requires(const T& value) {
  { ::cvt::test::Printer<T>::to_string(value) } -> std::convertible_to<std::string>;
};
template <class T>
concept StringLike = std::is_convertible_v<const T&, std::string_view>;
template <class T>
concept Utf8StringLike = std::is_convertible_v<const T&, std::u8string_view>;
template <class T>
concept Utf16StringLike = std::is_convertible_v<const T&, std::u16string_view>;
template <class T>
concept Utf32StringLike = std::is_convertible_v<const T&, std::u32string_view>;
template <class T>
concept WideStringLike = std::is_convertible_v<const T&, std::wstring_view>;
template <class T>
concept PathLike = requires(const T& value) {
  value.generic_u8string();
  value.root_name();
  value.has_filename();
};
template <class T>
concept DurationLike = requires(const T& value) {
  typename T::rep;
  value.count();
  T::period::num;
  T::period::den;
};
template <class T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };
template <class T>
concept ExpectedLike = requires(const T& value) {
  { value.has_value() } -> std::convertible_to<bool>;
  value.error();
};
template <class T>
concept OptionalLike = requires(const T& value) {
  { value.has_value() } -> std::convertible_to<bool>;
  requires !std::is_void_v<decltype(*value)>;
};
template <class T>
concept SmartPointerLike = requires(const T& value) {
  value.operator->();
  requires std::is_pointer_v<decltype(value.get())>;
};
template <class T>
concept VariantLike = requires(const T& value) {
  value.index();
  value.valueless_by_exception();
};
template <class T>
concept RangeLike = requires(const T& value) {
  adl::begin_of(value) != adl::end_of(value);
  *adl::begin_of(value);
};
template <class T>
concept ContiguousSized = requires(const T& value) {
  { std::data(value) } -> std::convertible_to<const void*>;
  { std::size(value) } -> std::convertible_to<std::size_t>;
};
template <class T>
concept TupleLike = requires { std::tuple_size<T>::value; };
template <class T>
concept Complete = requires { sizeof(T); };

template <class T>
using ElementOf = std::remove_cvref_t<decltype(*adl::begin_of(std::declval<const T&>()))>;
template <class T>
concept ByteRun = RangeLike<T> && ContiguousSized<T> && std::same_as<ElementOf<T>, std::byte>;

inline constexpr std::size_t kMaxElements = 16;

template <class T>
void put_enum(std::string& out, const T& value) {
  std::string number;
  if constexpr (std::is_signed_v<std::underlying_type_t<T>>) {
    append_signed(number, static_cast<long long>(value));
  } else {
    append_unsigned(number, static_cast<unsigned long long>(value));
  }
  if constexpr (Streamable<T>) {
    // An unscoped enum streams as its number unless it has its own operator<<.
    StreamCapture capture;
    capture.stream() << value;
    if (!capture.text().empty() && capture.text() != number) {
      out += capture.text();
      out += " (";
      out += number;
      out += ')';
      return;
    }
  }
  out += number;
}

template <class T>
void put_pointer(std::string& out, const T& value) {
  using Pointee = std::remove_cv_t<std::remove_pointer_t<T>>;
  if (value == nullptr) {
    out += "nullptr";
  } else if constexpr (std::is_same_v<Pointee, char>) {
    append_quoted(out, std::string_view(value));
  } else if constexpr (std::is_function_v<Pointee>) {
    out += "{function}";
  } else {
    append_pointer(out, const_cast<const void*>(static_cast<const volatile void*>(value)));
  }
}

template <class T>
void put_range(std::string& out, const T& range) {
  if constexpr (ByteRun<T>) {
    append_bytes(out, std::data(range), static_cast<std::size_t>(std::size(range)));
  } else {
    auto it = adl::begin_of(range);
    const auto last = adl::end_of(range);
    std::size_t count = 0;
    out += '{';
    for (; it != last && count < kMaxElements; ++it, ++count) {
      if (count != 0) out += ", ";
      fw::put(out, *it);
    }
    if (it != last) {
      for (; it != last; ++it) ++count;
      out += ", ... (";
      append_unsigned(out, count);
      out += " elements)";
    }
    out += '}';
  }
}

template <class T, std::size_t... I>
void put_tuple(std::string& out, const T& value, std::index_sequence<I...>) {
  using std::get;
  out += '{';
  ((out += (I == 0 ? "" : ", "), fw::put(out, get<I>(value))), ...);
  out += '}';
  (void)value;
}

template <class T>
void put_expected(std::string& out, const T& value) {
  if (value.has_value()) {
    if constexpr (requires { requires !std::is_void_v<decltype(*value)>; }) {
      fw::put(out, *value);
    } else {
      out += "{void}";
    }
    return;
  }
  std::string error;
  fw::put(error, value.error());
  out += "error";
  if (error.starts_with('{')) {
    out += error;
  } else {
    out += '{';
    out += error;
    out += '}';
  }
}

template <class T>
void put(std::string& out, const T& value) {
  (void)value;  // every use may sit in a discarded branch
  if constexpr (HasPrinter<T>) {
    out += ::cvt::test::Printer<T>::to_string(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    out += value ? "true" : "false";
  } else if constexpr (std::is_null_pointer_v<T>) {
    out += "nullptr";
  } else if constexpr (std::is_same_v<T, char>) {
    append_quoted_char(out, value);
  } else if constexpr (std::is_same_v<T, char8_t>) {
    append_quoted_char(out, static_cast<char>(value));
  } else if constexpr (std::is_same_v<T, wchar_t> || std::is_same_v<T, char16_t> || std::is_same_v<T, char32_t>) {
    append_code_point(out, static_cast<char32_t>(value));
  } else if constexpr (std::is_integral_v<T>) {
    if constexpr (std::is_signed_v<T>) {
      append_signed(out, static_cast<long long>(value));
    } else {
      append_unsigned(out, static_cast<unsigned long long>(value));
    }
  } else if constexpr (std::is_same_v<T, float>) {
    append_float(out, value);
  } else if constexpr (std::is_same_v<T, double>) {
    append_double(out, value);
  } else if constexpr (std::is_same_v<T, long double>) {
    append_long_double(out, value);
  } else if constexpr (std::is_same_v<T, std::byte>) {
    append_hex_byte(out, static_cast<unsigned>(value));
  } else if constexpr (std::is_enum_v<T>) {
    put_enum(out, value);
  } else if constexpr (std::is_function_v<T>) {
    out += "{function}";
  } else if constexpr (std::is_pointer_v<T>) {
    put_pointer(out, value);
  } else if constexpr (std::is_array_v<T> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, char>) {
    // Not necessarily terminated: stop at the first NUL or at the end of the array.
    std::size_t length = 0;
    while (length < std::extent_v<T> && value[length] != '\0') ++length;
    append_quoted(out, std::string_view(value, length));
  } else if constexpr (StringLike<T>) {
    append_quoted(out, std::string_view(value));
  } else if constexpr (Utf8StringLike<T>) {
    const std::u8string_view text(value);
    append_quoted(out, std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
  } else if constexpr (Utf16StringLike<T>) {
    const std::u16string_view text(value);
    append_quoted_wide(out, text.data(), text.size(), sizeof(char16_t));
  } else if constexpr (Utf32StringLike<T>) {
    const std::u32string_view text(value);
    append_quoted_wide(out, text.data(), text.size(), sizeof(char32_t));
  } else if constexpr (WideStringLike<T>) {
    const std::wstring_view text(value);
    append_quoted_wide(out, text.data(), text.size(), sizeof(wchar_t));
  } else if constexpr (std::is_array_v<T>) {
    put_range(out, value);
  } else if constexpr (PathLike<T>) {
    const auto text = value.generic_u8string();
    append_quoted(out, std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
  } else if constexpr (DurationLike<T>) {
    fw::put(out, value.count());
    append_duration_unit(out, static_cast<long long>(T::period::num), static_cast<long long>(T::period::den));
  } else if constexpr (SmartPointerLike<T>) {
    put_pointer(out, value.get());
  } else if constexpr (Streamable<T>) {
    StreamCapture capture;
    capture.stream() << value;
    out += capture.text();
  } else if constexpr (ExpectedLike<T>) {
    put_expected(out, value);
  } else if constexpr (OptionalLike<T>) {
    if (value.has_value()) {
      fw::put(out, *value);
    } else {
      out += "nullopt";
    }
  } else if constexpr (VariantLike<T>) {
    if (value.valueless_by_exception()) {
      out += "{valueless variant}";
    } else {
      visit([&out](const auto& alternative) { fw::put(out, alternative); }, value);
    }
  } else if constexpr (RangeLike<T>) {
    put_range(out, value);
  } else if constexpr (TupleLike<T>) {
    put_tuple(out, value, std::make_index_sequence<std::tuple_size<T>::value>{});
  } else {
    bool is_empty = false;
    if constexpr (Complete<T>) is_empty = std::is_empty_v<T>;
    append_type_name(out, signature_of<T>(), signature_of<double>(), is_empty);
  }
}

/// A string-like operand as a view; nullopt-like "no view" for a null `const char*`.
template <class T>
[[nodiscard]] bool view_of(const T& value, std::string_view& view) {
  if constexpr (std::is_pointer_v<T>) {
    if (value == nullptr) return false;
  }
  view = std::string_view(value);
  return true;
}

/// Extra help for a failed equality: where two strings, byte runs or ranges first differ.
template <class L, class R>
[[nodiscard]] std::string difference_note(const L& lhs, const R& rhs) {
  (void)lhs;  // every use may sit in a discarded branch
  (void)rhs;
  if constexpr (StringLike<L> && StringLike<R>) {
    std::string_view a;
    std::string_view b;
    if (!view_of(lhs, a) || !view_of(rhs, b)) return {};
    return string_difference(a, b);
  } else if constexpr (ByteRun<L> && ByteRun<R>) {
    return bytes_difference(std::data(lhs), static_cast<std::size_t>(std::size(lhs)), std::data(rhs),
                            static_cast<std::size_t>(std::size(rhs)));
  } else if constexpr (RangeLike<L> && RangeLike<R> && !PathLike<L> && !PathLike<R>) {
    if constexpr (requires(const ElementOf<L>& a, const ElementOf<R>& b) { static_cast<bool>(a == b); }) {
      auto a = adl::begin_of(lhs);
      auto b = adl::begin_of(rhs);
      const auto a_end = adl::end_of(lhs);
      const auto b_end = adl::end_of(rhs);
      std::size_t index = 0;
      for (; a != a_end && b != b_end; ++a, ++b, ++index) {
        if (static_cast<bool>(*a == *b)) continue;
        std::string note = "first difference at index ";
        append_unsigned(note, index);
        note += ": ";
        fw::put(note, *a);
        note += " != ";
        fw::put(note, *b);
        return note;
      }
      if (a == a_end && b == b_end) return {};
      std::size_t lhs_size = index;
      std::size_t rhs_size = index;
      for (; a != a_end; ++a) ++lhs_size;
      for (; b != b_end; ++b) ++rhs_size;
      std::string note = "sizes differ: ";
      append_unsigned(note, lhs_size);
      note += " and ";
      append_unsigned(note, rhs_size);
      note += " elements; the first ";
      append_unsigned(note, index);
      note += " are equal";
      return note;
    } else {
      return {};
    }
  } else {
    return {};
  }
}

// ---------------------------------------------------------------------------
// Messages: INFO, CAPTURE, *_MESSAGE, FAIL, MESSAGE
// ---------------------------------------------------------------------------

template <class T>
void append_part(std::string& out, const T& part) {
  if constexpr (std::is_same_v<T, char>) {
    out += part;
  } else if constexpr (StringLike<T> && !HasPrinter<T>) {
    std::string_view view;
    if (view_of(part, view)) {
      out += view;
    } else {
      out += "nullptr";
    }
  } else {
    fw::put(out, part);
  }
}

template <class... Parts>
void append_message(std::string& out, const Parts&... parts) {
  (fw::append_part(out, parts), ...);
  (void)out;
}

template <class... Parts>
[[nodiscard]] std::string message_text(const Parts&... parts) {
  std::string out;
  fw::append_message(out, parts...);
  return out;
}

template <class T>
void append_capture(std::string& out, const char* name, const T& value) {
  out += name;
  out += " := ";
  fw::put(out, value);
}

/// One entry of the per-thread stack of INFO / CAPTURE scopes.
struct InfoNode {
  InfoNode* previous = nullptr;
  void (*render)(const InfoNode&, std::string&) = nullptr;
  int uncaught = 0;
};
void info_push(InfoNode& node) noexcept;
void info_pop(InfoNode& node) noexcept;

template <class F>
class ScopedInfo final : private InfoNode {
 public:
  explicit ScopedInfo(const F& function) : function_(function) {
    render = [](const InfoNode& node, std::string& out) { static_cast<const ScopedInfo&>(node).function_(out); };
    info_push(*this);
  }
  ~ScopedInfo() { info_pop(*this); }
  ScopedInfo(const ScopedInfo&) = delete;
  ScopedInfo& operator=(const ScopedInfo&) = delete;

 private:
  F function_;
};

// ---------------------------------------------------------------------------
// Expression decomposition
// ---------------------------------------------------------------------------
//
// CHECK(a == b) expands to check(site, flags, Decomposer{} <=> a == b).
// `<=>` binds tighter than the comparison operators and looser than
// arithmetic and shifts, so the left operand of the comparison ends up in an
// Lhs<> and Lhs<>::operator== sees the right one. Operands are held by
// reference (string literals decay to `const char*`) for the duration of the
// full expression only.

/// A decomposed comparison: both operands and its result.
template <class L, class R>
struct Binary {
  L lhs;  ///< `const X&`, or `const char*` for a string literal
  R rhs;
  Op op;
  bool result;

  [[nodiscard]] explicit operator bool() const noexcept { return result; }
  /// True when the operands were found unequal by == or !=.
  [[nodiscard]] bool unequal() const noexcept { return (op == Op::Eq && !result) || (op == Op::Ne && result); }
};

// The comparisons below are the user's own, moved into a template: silence
// the warnings that literal operands would not have triggered at the call site.
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wsign-compare"
#  pragma clang diagnostic ignored "-Wfloat-equal"
#  pragma clang diagnostic ignored "-Wdouble-promotion"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wfloat-equal"
#  pragma GCC diagnostic ignored "-Wdouble-promotion"
#elif defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4018 4388 4389 4805)
#endif

#define CVT_TEST_DETAIL_COMPARISON(op, kind)                                                                \
  template <class R>                                                                                        \
  [[nodiscard]] Binary<T, const R&> operator op(const R& rhs) const {                                       \
    return {value, rhs, kind, static_cast<bool>(value op rhs)};                                             \
  }                                                                                                         \
  template <std::size_t N>                                                                                  \
    requires requires(const Value& lhs, const char* text) { static_cast<bool>(lhs op text); }              \
  [[nodiscard]] Binary<T, const char*> operator op(const char (&rhs)[N]) const {                            \
    const char* const text = rhs;                                                                           \
    return {value, text, kind, static_cast<bool>(value op text)};                                           \
  }

#define CVT_TEST_DETAIL_BITWISE(op)                                                                         \
  template <class R>                                                                                        \
  [[nodiscard]] auto operator op(const R& rhs) const                                                        \
      -> Lhs<std::remove_cvref_t<decltype(std::declval<const Value&>() op rhs)>> {                          \
    return {value op rhs};                                                                                  \
  }

/// The left operand of an expression under test.
template <class T>
struct Lhs {
  using Value = std::remove_cvref_t<T>;
  T value;  ///< `const X&`, a decayed `const char*`, or the value of a bitwise sub-expression

  CVT_TEST_DETAIL_COMPARISON(==, Op::Eq)
  CVT_TEST_DETAIL_COMPARISON(!=, Op::Ne)
  CVT_TEST_DETAIL_COMPARISON(<, Op::Lt)
  CVT_TEST_DETAIL_COMPARISON(<=, Op::Le)
  CVT_TEST_DETAIL_COMPARISON(>, Op::Gt)
  CVT_TEST_DETAIL_COMPARISON(>=, Op::Ge)
  CVT_TEST_DETAIL_BITWISE(&)
  CVT_TEST_DETAIL_BITWISE(|)
  CVT_TEST_DETAIL_BITWISE(^)

  /// Lets `CHECK(a && b)`, `CHECK(a || b)` and `CHECK(a ? b : c)` use the built-in operators.
  [[nodiscard]] explicit operator bool() const
    requires requires(const Value& operand) { static_cast<bool>(operand); }
  {
    return static_cast<bool>(value);
  }
};

#undef CVT_TEST_DETAIL_COMPARISON
#undef CVT_TEST_DETAIL_BITWISE

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif

struct Decomposer {
  template <class T>
  [[nodiscard]] constexpr Lhs<const T&> operator<=>(const T& value) const noexcept {
    return {value};
  }
  template <std::size_t N>
  [[nodiscard]] constexpr Lhs<const char*> operator<=>(const char (&value)[N]) const noexcept {
    return {value};
  }
};

template <class T>
inline constexpr bool is_decomposed = false;
template <class T>
inline constexpr bool is_decomposed<Lhs<T>> = true;
template <class L, class R>
inline constexpr bool is_decomposed<Binary<L, R>> = true;

/// Unary case: `CHECK(value)`, and what `&&`, `||` and `?:` leave behind.
template <class T>
void check_value(const Site& site, unsigned flags, const T& value, LazyText detail) {
  const bool ok = static_cast<bool>(value) != ((flags & kNegate) != 0u);
  if (fast_pass(ok)) return;
  std::string expansion;
  if constexpr (!std::is_same_v<T, bool>) fw::put(expansion, value);
  report_assertion(site, flags, ok, expansion, {}, detail);
}

template <class T>
void check(const Site& site, unsigned flags, const Lhs<T>& expression, LazyText detail = {}) {
  check_value(site, flags, expression.value, detail);
}

template <class L, class R>
void check(const Site& site, unsigned flags, const Binary<L, R>& expression, LazyText detail = {}) {
  const bool ok = expression.result != ((flags & kNegate) != 0u);
  if (fast_pass(ok)) return;
  std::string lhs;
  std::string rhs;
  std::string note;
  fw::put(lhs, expression.lhs);
  fw::put(rhs, expression.rhs);
  if (!ok && expression.unequal()) note = difference_note(expression.lhs, expression.rhs);
  report_comparison(site, flags, ok, lhs, expression.op, rhs, note, detail);
}

template <class T>
  requires(!is_decomposed<T>)
void check(const Site& site, unsigned flags, const T& value, LazyText detail = {}) {
  check_value(site, flags, value, detail);
}

[[nodiscard]] constexpr bool is_finite_number(double value) noexcept {
  return value >= std::numeric_limits<double>::lowest() && value <= std::numeric_limits<double>::max();
}

/// |lhs - rhs| <= tolerance; NaN equals nothing and an infinity only itself.
[[nodiscard]] constexpr bool approx_equal(double lhs, double rhs, double tolerance) noexcept {
  if (!is_finite_number(lhs) || !is_finite_number(rhs)) return lhs == rhs;
  const double difference = lhs > rhs ? lhs - rhs : rhs - lhs;
  return difference <= tolerance;
}

template <class A, class B, class T>
void check_approx(const Site& site, unsigned flags, const A& lhs, const B& rhs, const T& tolerance) {
  const double a = static_cast<double>(lhs);
  const double b = static_cast<double>(rhs);
  const double t = static_cast<double>(tolerance);
  const bool ok = approx_equal(a, b, t);
  if (fast_pass(ok)) return;
  report_approx(site, flags, ok, a, b, t);
}

template <class R>
void check_ok(const Site& site, unsigned flags, const R& result) {
  const bool ok = result.has_value();
  if (fast_pass(ok)) return;
  std::string expansion;
  if (ok) {
    expansion = "holds a value";
  } else {
    put_expected(expansion, result);
  }
  report_assertion(site, flags, ok, expansion, {}, {});
}

template <class R>
void check_status(const Site& site, unsigned flags, const R& result, Status expected) {
  const bool ok = !result.has_value() && result.error().code == expected;
  if (fast_pass(ok)) return;
  std::string expansion;
  if (result.has_value()) {
    expansion = "holds a value";
  } else {
    put_expected(expansion, result);
  }
  expansion += ok ? " (as expected)" : " (expected status: ";
  if (!ok) {
    expansion += status_name(expected);
    expansion += ')';
  }
  report_assertion(site, flags, ok, expansion, {}, {});
}

// ---------------------------------------------------------------------------
// Test cases and subcases
// ---------------------------------------------------------------------------

using TestFunction = void (*)();
int register_test_case(TestFunction function, const char* name, const char* file, int line) noexcept;

class Subcase {
 public:
  Subcase(std::string_view name, const char* file, int line);
  ~Subcase();
  Subcase(const Subcase&) = delete;
  Subcase& operator=(const Subcase&) = delete;
  [[nodiscard]] explicit operator bool() const noexcept { return entered_; }

 private:
  bool entered_ = false;
  int uncaught_ = 0;
};

// ---------------------------------------------------------------------------
// Running a body as an isolated test case (for the framework's own tests)
// ---------------------------------------------------------------------------

struct IsolatedOptions {
  bool success = false;          ///< like --success
  bool absolute_paths = false;   ///< like --absolute-paths
  std::uint64_t abort_after = 0; ///< like --abort-after
};

struct IsolatedResult {
  std::uint64_t passed = 0;      ///< passing assertions
  std::uint64_t failed = 0;      ///< failing assertions
  std::uint64_t exceptions = 0;  ///< unexpected exceptions
  unsigned paths = 0;            ///< times the body was run and its results kept
  bool skipped = false;
  std::string output;            ///< everything that would have been printed, without colours

  [[nodiscard]] bool ok() const noexcept { return failed == 0 && exceptions == 0; }
};

[[nodiscard]] IsolatedResult run_isolated_impl(void (*invoke)(void*), void* object, const IsolatedOptions& options,
                                               std::string_view name, const char* file, int line);

/// Runs `body` exactly like a test case (subcases, exception capture, INFO
/// scopes) but with its own counters and output; nothing it does is counted
/// in or printed by the surrounding run.
template <class F>
[[nodiscard]] IsolatedResult run_isolated(F&& body, const IsolatedOptions& options = {},
                                          std::string_view name = "isolated",
                                          std::source_location where = std::source_location::current()) {
  auto* object = &body;
  return run_isolated_impl([](void* opaque) { (*static_cast<decltype(object)>(opaque))(); },
                           const_cast<void*>(static_cast<const void*>(object)), options, name, where.file_name(),
                           static_cast<int>(where.line()));
}

/// `*` matches any run of characters, `?` exactly one; the whole text must match.
[[nodiscard]] bool wildcard_match(std::string_view pattern, std::string_view text) noexcept;
/// True when `text` matches one of the comma-separated patterns of `filter` (`\,` is a literal comma).
[[nodiscard]] bool filter_matches(std::string_view filter, std::string_view text);
/// The file name as printed in reports: relative to the repository.
[[nodiscard]] std::string display_path(std::string_view file);

enum class Selection { Selected, NotSelected, UsageError };
/// Whether the runner, started with `arguments`, would run a test case called `name` defined in `file`.
[[nodiscard]] Selection selects(std::initializer_list<std::string_view> arguments, std::string_view name,
                                std::string_view file);

}  // namespace cvt::test::fw

namespace cvt::test {

template <class T>
[[nodiscard]] std::string stringify(const T& value) {
  std::string out;
  fw::put(out, value);
  return out;
}

/// A number with a tolerance, for `CHECK(x == approx(1.5).abs(1e-9))`.
///   approx(v)            |x - v| <= max(1e-12, 1e-9 * max(|x|, |v|))
///   approx(v).abs(t)     |x - v| <= t
///   approx(v).rel(r)     |x - v| <= r * max(|x|, |v|)
///   approx(v).abs(t).rel(r)   either of the two
/// NaN never compares equal; an infinity only equals itself.
class Approx {
 public:
  constexpr explicit Approx(double value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Approx abs(double tolerance) const noexcept {
    Approx copy = *this;
    copy.abs_ = tolerance > 0.0 ? tolerance : 0.0;
    return copy;
  }
  [[nodiscard]] constexpr Approx rel(double tolerance) const noexcept {
    Approx copy = *this;
    copy.rel_ = tolerance > 0.0 ? tolerance : 0.0;
    return copy;
  }

  [[nodiscard]] constexpr double value() const noexcept { return value_; }
  [[nodiscard]] constexpr double abs_tolerance() const noexcept {
    return abs_ < 0.0 ? (rel_ < 0.0 ? 1e-12 : 0.0) : abs_;
  }
  [[nodiscard]] constexpr double rel_tolerance() const noexcept {
    return rel_ < 0.0 ? (abs_ < 0.0 ? 1e-9 : 0.0) : rel_;
  }

  [[nodiscard]] constexpr bool matches(double other) const noexcept {
    if (!fw::is_finite_number(other) || !fw::is_finite_number(value_)) return other == value_;
    const double a = other < 0.0 ? -other : other;
    const double b = value_ < 0.0 ? -value_ : value_;
    const double relative = rel_tolerance() * (a > b ? a : b);
    const double absolute = abs_tolerance();
    return fw::approx_equal(other, value_, absolute > relative ? absolute : relative);
  }

  template <class T>
    requires std::is_arithmetic_v<T>
  [[nodiscard]] friend constexpr bool operator==(const T& lhs, const Approx& rhs) noexcept {
    return rhs.matches(static_cast<double>(lhs));
  }
  template <class T>
    requires std::is_arithmetic_v<T>
  [[nodiscard]] friend constexpr bool operator<=(const T& lhs, const Approx& rhs) noexcept {
    return static_cast<double>(lhs) < rhs.value_ || rhs.matches(static_cast<double>(lhs));
  }
  template <class T>
    requires std::is_arithmetic_v<T>
  [[nodiscard]] friend constexpr bool operator>=(const T& lhs, const Approx& rhs) noexcept {
    return static_cast<double>(lhs) > rhs.value_ || rhs.matches(static_cast<double>(lhs));
  }
  template <class T>
    requires std::is_arithmetic_v<T>
  [[nodiscard]] friend constexpr bool operator<=(const Approx& lhs, const T& rhs) noexcept {
    return rhs >= lhs;
  }
  template <class T>
    requires std::is_arithmetic_v<T>
  [[nodiscard]] friend constexpr bool operator>=(const Approx& lhs, const T& rhs) noexcept {
    return rhs <= lhs;
  }

 private:
  double value_;
  double abs_ = -1.0;  ///< negative: not set
  double rel_ = -1.0;
};

[[nodiscard]] constexpr Approx approx(double value) noexcept { return Approx(value); }

template <>
struct Printer<Approx> {
  [[nodiscard]] static std::string to_string(const Approx& value) {
    std::string out = "approx(";
    fw::append_double(out, value.value());
    out += ", abs ";
    fw::append_double(out, value.abs_tolerance());
    out += ", rel ";
    fw::append_double(out, value.rel_tolerance());
    out += ')';
    return out;
  }
};

/// Statuses print by name in every translation unit, whether or not an
/// operator<< for them is in sight.
template <>
struct Printer<Status> {
  [[nodiscard]] static std::string to_string(Status value) { return std::string(status_name(value)); }
};

template <>
struct Printer<Error> {
  [[nodiscard]] static std::string to_string(const Error& value) {
    std::string out = "{";
    out += status_name(value.code);
    out += ", ";
    fw::append_quoted(out, value.message);
    out += '}';
    return out;
  }
};

namespace detail {
// The framework's own tests reach these through the documented name.
using fw::IsolatedOptions;
using fw::IsolatedResult;
using fw::run_isolated;
}  // namespace detail

}  // namespace cvt::test

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define CVT_TEST_DETAIL_CAT2(a, b) a##b
#define CVT_TEST_DETAIL_CAT(a, b) CVT_TEST_DETAIL_CAT2(a, b)
#define CVT_TEST_DETAIL_UNIQUE(prefix) CVT_TEST_DETAIL_CAT(prefix, __COUNTER__)
#define CVT_TEST_DETAIL_SITE(macro, text) \
  ::cvt::test::fw::Site { __FILE__, __LINE__, macro, text }

#define CVT_TEST_DETAIL_TEST_CASE(function, name)                                                              \
  static void function();                                                                                      \
  [[maybe_unused]] static const int CVT_TEST_DETAIL_CAT(function, _registered) =                               \
      ::cvt::test::fw::register_test_case(&function, name, __FILE__, __LINE__);                                \
  static void function()

#define CVT_TEST_DETAIL_ASSERT(macro, flags, ...)                           \
  ::cvt::test::fw::check(CVT_TEST_DETAIL_SITE(macro, #__VA_ARGS__), flags, \
                         ::cvt::test::fw::Decomposer{} <=> __VA_ARGS__)

#define CVT_TEST_DETAIL_ASSERT_MESSAGE(macro, flags, expression, ...)                                          \
  ::cvt::test::fw::check(CVT_TEST_DETAIL_SITE(macro, #expression), flags,                                      \
                         ::cvt::test::fw::Decomposer{} <=> expression, [&](::std::string& cvt_test_out_) {     \
                           ::cvt::test::fw::append_message(cvt_test_out_, __VA_ARGS__);                        \
                         })

#define CVT_TEST_DETAIL_COMPARE(macro, flags, op, lhs, rhs)                                                    \
  ::cvt::test::fw::check(CVT_TEST_DETAIL_SITE(macro, #lhs ", " #rhs), flags,                                   \
                         (::cvt::test::fw::Decomposer{} <=> (lhs)) op (rhs))

#define TEST_CASE(name) CVT_TEST_DETAIL_TEST_CASE(CVT_TEST_DETAIL_UNIQUE(cvt_test_case_), name)

#define SUBCASE(name) \
  if (const ::cvt::test::fw::Subcase CVT_TEST_DETAIL_UNIQUE(cvt_test_subcase_){name, __FILE__, __LINE__})

#define CHECK(...) CVT_TEST_DETAIL_ASSERT("CHECK", 0u, __VA_ARGS__)
#define CHECK_FALSE(...) CVT_TEST_DETAIL_ASSERT("CHECK_FALSE", ::cvt::test::fw::kNegate, __VA_ARGS__)
#define REQUIRE(...) CVT_TEST_DETAIL_ASSERT("REQUIRE", ::cvt::test::fw::kFatal, __VA_ARGS__)
#define REQUIRE_FALSE(...) \
  CVT_TEST_DETAIL_ASSERT("REQUIRE_FALSE", ::cvt::test::fw::kFatal | ::cvt::test::fw::kNegate, __VA_ARGS__)

#define CHECK_MESSAGE(expression, ...) CVT_TEST_DETAIL_ASSERT_MESSAGE("CHECK", 0u, expression, __VA_ARGS__)
#define CHECK_FALSE_MESSAGE(expression, ...) \
  CVT_TEST_DETAIL_ASSERT_MESSAGE("CHECK_FALSE", ::cvt::test::fw::kNegate, expression, __VA_ARGS__)
#define REQUIRE_MESSAGE(expression, ...) \
  CVT_TEST_DETAIL_ASSERT_MESSAGE("REQUIRE", ::cvt::test::fw::kFatal, expression, __VA_ARGS__)
#define REQUIRE_FALSE_MESSAGE(expression, ...)                                                                 \
  CVT_TEST_DETAIL_ASSERT_MESSAGE("REQUIRE_FALSE", ::cvt::test::fw::kFatal | ::cvt::test::fw::kNegate,          \
                                 expression, __VA_ARGS__)

#define CHECK_EQ(lhs, rhs) CVT_TEST_DETAIL_COMPARE("CHECK_EQ", 0u, ==, lhs, rhs)
#define CHECK_NE(lhs, rhs) CVT_TEST_DETAIL_COMPARE("CHECK_NE", 0u, !=, lhs, rhs)
#define CHECK_LT(lhs, rhs) CVT_TEST_DETAIL_COMPARE("CHECK_LT", 0u, <, lhs, rhs)
#define CHECK_LE(lhs, rhs) CVT_TEST_DETAIL_COMPARE("CHECK_LE", 0u, <=, lhs, rhs)
#define CHECK_GT(lhs, rhs) CVT_TEST_DETAIL_COMPARE("CHECK_GT", 0u, >, lhs, rhs)
#define CHECK_GE(lhs, rhs) CVT_TEST_DETAIL_COMPARE("CHECK_GE", 0u, >=, lhs, rhs)
#define REQUIRE_EQ(lhs, rhs) CVT_TEST_DETAIL_COMPARE("REQUIRE_EQ", ::cvt::test::fw::kFatal, ==, lhs, rhs)
#define REQUIRE_NE(lhs, rhs) CVT_TEST_DETAIL_COMPARE("REQUIRE_NE", ::cvt::test::fw::kFatal, !=, lhs, rhs)
#define REQUIRE_LT(lhs, rhs) CVT_TEST_DETAIL_COMPARE("REQUIRE_LT", ::cvt::test::fw::kFatal, <, lhs, rhs)
#define REQUIRE_LE(lhs, rhs) CVT_TEST_DETAIL_COMPARE("REQUIRE_LE", ::cvt::test::fw::kFatal, <=, lhs, rhs)
#define REQUIRE_GT(lhs, rhs) CVT_TEST_DETAIL_COMPARE("REQUIRE_GT", ::cvt::test::fw::kFatal, >, lhs, rhs)
#define REQUIRE_GE(lhs, rhs) CVT_TEST_DETAIL_COMPARE("REQUIRE_GE", ::cvt::test::fw::kFatal, >=, lhs, rhs)

#define CHECK_APPROX(lhs, rhs, tolerance)                                                                      \
  ::cvt::test::fw::check_approx(CVT_TEST_DETAIL_SITE("CHECK_APPROX", #lhs ", " #rhs ", " #tolerance), 0u, lhs, \
                                rhs, tolerance)
#define REQUIRE_APPROX(lhs, rhs, tolerance)                                                                    \
  ::cvt::test::fw::check_approx(CVT_TEST_DETAIL_SITE("REQUIRE_APPROX", #lhs ", " #rhs ", " #tolerance),        \
                                ::cvt::test::fw::kFatal, lhs, rhs, tolerance)

#define CHECK_OK(...) ::cvt::test::fw::check_ok(CVT_TEST_DETAIL_SITE("CHECK_OK", #__VA_ARGS__), 0u, __VA_ARGS__)
#define REQUIRE_OK(...) \
  ::cvt::test::fw::check_ok(CVT_TEST_DETAIL_SITE("REQUIRE_OK", #__VA_ARGS__), ::cvt::test::fw::kFatal, __VA_ARGS__)
#define CHECK_STATUS(result, status) \
  ::cvt::test::fw::check_status(CVT_TEST_DETAIL_SITE("CHECK_STATUS", #result ", " #status), 0u, result, status)
#define REQUIRE_STATUS(result, status)                                                                         \
  ::cvt::test::fw::check_status(CVT_TEST_DETAIL_SITE("REQUIRE_STATUS", #result ", " #status),                  \
                                ::cvt::test::fw::kFatal, result, status)

#define INFO(...)                                                                                              \
  const ::cvt::test::fw::ScopedInfo CVT_TEST_DETAIL_UNIQUE(cvt_test_info_)([&](::std::string& cvt_test_out_) { \
    ::cvt::test::fw::append_message(cvt_test_out_, __VA_ARGS__);                                               \
  })
#define CAPTURE(...)                                                                                           \
  const ::cvt::test::fw::ScopedInfo CVT_TEST_DETAIL_UNIQUE(cvt_test_info_)([&](::std::string& cvt_test_out_) { \
    ::cvt::test::fw::append_capture(cvt_test_out_, #__VA_ARGS__, __VA_ARGS__);                                 \
  })

#define MESSAGE(...) ::cvt::test::fw::message(__FILE__, __LINE__, ::cvt::test::fw::message_text(__VA_ARGS__))
#define FAIL(...) ::cvt::test::fw::fail_fatal(__FILE__, __LINE__, ::cvt::test::fw::message_text(__VA_ARGS__))
#define FAIL_CHECK(...) ::cvt::test::fw::fail_soft(__FILE__, __LINE__, ::cvt::test::fw::message_text(__VA_ARGS__))
#define SKIP(...) ::cvt::test::fw::skip(__FILE__, __LINE__, ::cvt::test::fw::message_text(__VA_ARGS__))
#define CVT_TEST_FAIL_AT(file, line, ...) \
  ::cvt::test::fw::fail_soft(file, line, ::cvt::test::fw::message_text(__VA_ARGS__))

#endif  // CVT_TESTS_SUPPORT_TEST_HPP

// ============================================================================
// Implementation: compiled in exactly one translation unit
// ============================================================================
#if (defined(CVT_TEST_MAIN) || defined(CVT_TEST_IMPLEMENTATION)) && !defined(CVT_TESTS_SUPPORT_TEST_IMPLEMENTED)
#define CVT_TESTS_SUPPORT_TEST_IMPLEMENTED

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <streambuf>
#include <vector>

#if defined(__cpp_rtti) || defined(__GXX_RTTI) || defined(_CPPRTTI)
#  define CVT_TEST_DETAIL_RTTI 1
#  include <typeinfo>
#  if defined(__has_include)
#    if __has_include(<cxxabi.h>)
#      include <cxxabi.h>
#      define CVT_TEST_DETAIL_DEMANGLE 1
#    endif
#  endif
#endif

#if !defined(_WIN32)
#  include <unistd.h>
#endif

#if !defined(CVT_TEST_PROGRAM_NAME)
#  define CVT_TEST_PROGRAM_NAME "convertible_tests"
#endif

namespace cvt::test::fw {

namespace {

// ---- small text helpers ----------------------------------------------------

constexpr std::string_view kTag = "[" CVT_TEST_PROGRAM_NAME "] ";
constexpr std::size_t kLongString = 440;     ///< strings longer than this are cut in the middle ...
constexpr std::size_t kStringEdge = 200;     ///< ... keeping this many bytes at each end
constexpr std::size_t kDifferenceFrom = 40;  ///< strings at least this long get a first-difference note
constexpr std::size_t kMaxDumpBytes = 32;
constexpr std::size_t kInlineExpansion = 100;

template <class Number>
void append_number(std::string& out, Number value, int base = 10) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, base);
  out.append(buffer, result.ptr);
}

void append_hex2(std::string& out, unsigned value) {
  constexpr char digits[] = "0123456789abcdef";
  out += digits[(value >> 4) & 0xFu];
  out += digits[value & 0xFu];
}

template <class Float>
void append_floating(std::string& out, Float value, const char* suffix) {
  if (value != value) {
    out += "nan";
    return;
  }
  if (value - value != Float{0}) {
    out += value < Float{0} ? "-inf" : "inf";
    return;
  }
  // The fewest significant digits that read back as the same value.
  const auto parse = [](const char* text) -> Float {
    if constexpr (std::is_same_v<Float, float>) {
      return std::strtof(text, nullptr);
    } else if constexpr (std::is_same_v<Float, double>) {
      return std::strtod(text, nullptr);
    } else {
      return std::strtold(text, nullptr);
    }
  };
  const long double wide = static_cast<long double>(value);
  char buffer[80];
  int precision = 1;
  for (;; ++precision) {
    std::snprintf(buffer, sizeof(buffer), "%.*Lg", precision, wide);
    if (precision >= std::numeric_limits<Float>::max_digits10 || parse(buffer) == value) break;
  }
  // Plain notation for moderate magnitudes: 100000.0 rather than 1e+05.
  if (const char* exponent_at = std::strchr(buffer, 'e'); exponent_at != nullptr) {
    const int exponent = std::atoi(exponent_at + 1);
    if (exponent >= -5 && exponent < 16) {
      std::snprintf(buffer, sizeof(buffer), "%.*Lf", std::max(0, precision - 1 - exponent), wide);
    }
  }
  std::string text = buffer;
  std::replace(text.begin(), text.end(), ',', '.');  // a locale with a decimal comma
  if (text.find_first_of(".e") == std::string::npos) text += ".0";
  out += text;
  out += suffix;
}

/// Length of the well-formed UTF-8 sequence starting at `text[at]`, or 0.
std::size_t utf8_sequence(std::string_view text, std::size_t at) noexcept {
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned lead = byte(at);
  std::size_t length = 0;
  unsigned low = 0x80;
  unsigned high = 0xBF;
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    if (lead == 0xE0) low = 0xA0;
    if (lead == 0xED) high = 0x9F;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    if (lead == 0xF0) low = 0x90;
    if (lead == 0xF4) high = 0x8F;
  } else {
    return 0;
  }
  if (at + length > text.size()) return 0;
  if (byte(at + 1) < low || byte(at + 1) > high) return 0;
  for (std::size_t i = 2; i < length; ++i) {
    if ((byte(at + i) & 0xC0u) != 0x80u) return 0;
  }
  return length;
}

/// Appends `text` with C-style escapes; returns the number of display columns added.
std::size_t append_escaped(std::string& out, std::string_view text, char quote = '"') {
  std::size_t columns = 0;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0x80) {
      const std::size_t length = utf8_sequence(text, i);
      if (length == 0) {
        out += "\\x";
        append_hex2(out, c);
        columns += 4;
        ++i;
      } else {
        out.append(text.substr(i, length));
        columns += 1;
        i += length;
      }
      continue;
    }
    ++i;
    switch (c) {
      case '\n': out += "\\n"; columns += 2; continue;
      case '\r': out += "\\r"; columns += 2; continue;
      case '\t': out += "\\t"; columns += 2; continue;
      case '\0': out += "\\0"; columns += 2; continue;
      case '\\': out += "\\\\"; columns += 2; continue;
      default: break;
    }
    if (c == static_cast<unsigned char>(quote)) {
      out += '\\';
      out += quote;
      columns += 2;
    } else if (c < 0x20 || c == 0x7F) {
      out += "\\x";
      append_hex2(out, c);
      columns += 4;
    } else {
      out += static_cast<char>(c);
      columns += 1;
    }
  }
  return columns;
}

/// The nearest position <= `at` that does not split a UTF-8 sequence.
std::size_t utf8_floor(std::string_view text, std::size_t at) noexcept {
  std::size_t steps = 0;
  while (at > 0 && at < text.size() && steps < 3 && (static_cast<unsigned char>(text[at]) & 0xC0u) == 0x80u) {
    --at;
    ++steps;
  }
  return at;
}

void append_utf8(std::string& out, char32_t code_point) {
  if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) code_point = 0xFFFD;
  if (code_point < 0x80) {
    out += static_cast<char>(code_point);
  } else if (code_point < 0x800) {
    out += static_cast<char>(0xC0 | (code_point >> 6));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  } else if (code_point < 0x10000) {
    out += static_cast<char>(0xE0 | (code_point >> 12));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code_point >> 18));
    out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  }
}

void replace_all(std::string& text, std::string_view from, std::string_view to) {
  for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
}

/// Appends `text` with every line after the first indented by `indent` spaces.
void append_indented(std::string& out, std::string_view text, std::size_t indent) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
  for (const char c : text) {
    out += c;
    if (c == '\n') out.append(indent, ' ');
  }
}

// ---- run state -------------------------------------------------------------

struct AbortPath {};  ///< thrown by a failing REQUIRE / FAIL; never leaves the runner
struct SkipTest {};   ///< thrown by SKIP

struct Node {
  std::string name;
  const char* file = nullptr;
  int line = 0;
  bool complete = false;        ///< this subcase and everything below it has been run
  bool entered_before = false;  ///< entered in this or an earlier pass
  std::vector<std::unique_ptr<Node>> children;

  [[nodiscard]] bool children_complete() const noexcept {
    return std::all_of(children.begin(), children.end(), [](const auto& child) { return child->complete; });
  }
};

struct Frame {
  Node* node = nullptr;
  Node* entered_child = nullptr;  ///< the one child entered during this pass
};

struct ActiveTest {
  std::string name;
  const char* file = nullptr;
  int line = 0;

  Node root;
  std::vector<Frame> stack;      ///< stack[0] is the test case itself
  unsigned pass = 0;
  bool buffering = false;        ///< the pass may turn out to lead nowhere: hold its output
  bool entered_new = false;      ///< a subcase was entered for the first time in this pass
  bool entered_any = false;
  bool abort_marked = false;     ///< the subcase abandoned by an exception has been recorded
  std::string buffer;
  std::uint64_t pass_failed = 0;
  std::uint64_t pass_exceptions = 0;

  std::uint64_t failed = 0;      ///< results of the passes that were kept
  std::uint64_t exceptions = 0;
  unsigned paths = 0;
  bool skipped = false;

  std::vector<const Node*> path;  ///< the subcases entered in this pass, outermost first
  bool header_printed = false;
  std::size_t header_depth = 0;  ///< how much of `path` the printed header showed
};

struct Run {
  bool success = false;
  bool absolute_paths = false;
  bool colors = false;
  std::uint64_t abort_after = 0;
  std::FILE* file = nullptr;  ///< where output goes, or ...
  std::string captured;       ///< ... here when file is null

  std::uint64_t failed = 0;   ///< failed assertions of finished test cases
  bool stop = false;          ///< --abort-after was reached
  ActiveTest* test = nullptr;
};

std::recursive_mutex& state_mutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

Run* g_run = nullptr;  // guarded by state_mutex()
thread_local InfoNode* tl_info_head = nullptr;
thread_local bool tl_test_thread = false;
thread_local bool tl_has_unwound_context = false;

/// INFO text rendered while an exception unwinds, for the report of that exception.
std::vector<std::string>& unwound_context() {
  thread_local std::vector<std::string> context;
  return context;
}

struct Style {
  const char* red = "";
  const char* green = "";
  const char* yellow = "";
  const char* cyan = "";
  const char* bold = "";
  const char* reset = "";
};

Style style_for(const Run& run) noexcept {
  if (!run.colors) return {};
  return {"\x1b[1;31m", "\x1b[32m", "\x1b[33m", "\x1b[36m", "\x1b[1m", "\x1b[0m"};
}

void raw_write(Run& run, std::string_view text) {
  if (run.file != nullptr) {
    std::fwrite(text.data(), 1, text.size(), run.file);
    std::fflush(run.file);
  } else {
    run.captured += text;
  }
}

void write(Run& run, std::string_view text) {
  if (run.test != nullptr && run.test->buffering) {
    run.test->buffer += text;
  } else {
    raw_write(run, text);
  }
}

std::string shown_path(const Run& run, std::string_view file) {
  return run.absolute_paths ? std::string(file) : display_path(file);
}

void append_location(std::string& out, const Run& run, std::string_view file, int line) {
  out += shown_path(run, file);
  out += ':';
  append_number(out, line);
  out += ": ";
}

/// "TEST CASE: ..." and the active subcases, once per path (and again when the subcases change).
void append_header(std::string& out, Run& run) {
  ActiveTest* test = run.test;
  if (test == nullptr) return;
  if (test->header_printed && test->header_depth == test->path.size()) return;
  test->header_printed = true;
  test->header_depth = test->path.size();
  const Style style = style_for(run);
  out += style.bold;
  out += "TEST CASE: ";
  out += test->name;
  out += style.reset;
  out += "  [";
  out += shown_path(run, test->file);
  out += ':';
  append_number(out, test->line);
  out += "]\n";
  for (std::size_t depth = 0; depth < test->path.size(); ++depth) {
    out.append((depth + 1) * 2, ' ');
    out += "SUBCASE: ";
    out += test->path[depth]->name;
    out += '\n';
  }
}

void append_context(std::string& out) {
  std::vector<const InfoNode*> nodes;
  for (const InfoNode* node = tl_info_head; node != nullptr; node = node->previous) nodes.push_back(node);
  if (nodes.empty()) return;
  out += "  with context:\n";
  for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
    std::string line;
    (*it)->render(**it, line);
    out += "    ";
    append_indented(out, line, 4);
    out += '\n';
  }
}

[[noreturn]] void abort_off_thread(Run* run, std::string_view what) {
  std::string text(kTag);
  text += "FATAL: ";
  text += what;
  text += " on a thread that is not running the test case; aborting. Use CHECK in worker threads.\n";
  if (run != nullptr) {
    raw_write(*run, run->test != nullptr ? run->test->buffer : std::string());
    raw_write(*run, text);
  }
  std::fputs(text.c_str(), stderr);
  std::fflush(nullptr);
  std::abort();
}

/// Books one failed assertion; true when the run should stop (--abort-after).
bool count_failure(Run& run) {
  std::uint64_t total = run.failed + 1;
  if (run.test != nullptr) {
    ++run.test->pass_failed;
    total = run.failed + run.test->failed + run.test->pass_failed;
  } else {
    ++run.failed;
  }
  if (run.abort_after != 0 && total >= run.abort_after) run.stop = true;
  return run.stop;
}

enum class Verdict { Passed, Failed };

/// Prints one assertion block. `headline` is everything after "FAILED: ".
void emit_assertion(Run& run, const char* file, int line, Verdict verdict, std::string_view headline,
                    std::string_view expansion, std::string_view note, LazyText detail) {
  const Style style = style_for(run);
  std::string out;
  append_header(out, run);
  append_location(out, run, file, line);
  if (verdict == Verdict::Passed) {
    out += style.green;
    out += "PASSED";
  } else {
    out += style.red;
    out += "FAILED";
  }
  out += style.reset;
  out += ": ";
  append_indented(out, headline, 2);
  out += '\n';
  if (!expansion.empty()) {
    if (expansion.find('\n') == std::string_view::npos) {
      out += "  with expansion: ";
      out += expansion;
    } else {
      out += "  with expansion:\n    ";
      append_indented(out, expansion, 4);
    }
    out += '\n';
  }
  if (!note.empty()) {
    out += "  ";
    append_indented(out, note, 2);
    out += '\n';
  }
  if (detail) {
    std::string text;
    detail.render(text);
    if (!text.empty()) {
      out += "  message: ";
      append_indented(out, text, 4);
      out += '\n';
    }
  }
  append_context(out);
  if (verdict == Verdict::Failed) out += '\n';
  write(run, out);
}

void emit_orphan(const char* file, int line, std::string_view headline) {
  std::string out = display_path(file);
  out += ':';
  append_number(out, line);
  out += ": FAILED (outside any test case): ";
  out += headline;
  out += '\n';
  std::fputs(out.c_str(), stderr);
}

std::string assertion_headline(const Site& site) {
  std::string out = site.macro;
  out += "( ";
  out += site.text;
  out += " )";
  return out;
}

/// What a failing fatal assertion (or a reached --abort-after) does after its report.
void leave_path(Run* run, bool fatal, bool stop, std::string_view what) {
  if (!fatal && !stop) return;
  if (tl_test_thread) throw AbortPath{};
  if (fatal) {
    const std::lock_guard lock(state_mutex());
    abort_off_thread(run, what);
  }
}

constexpr std::string_view op_text(Op op) noexcept {
  switch (op) {
    case Op::Eq: return "==";
    case Op::Ne: return "!=";
    case Op::Lt: return "<";
    case Op::Le: return "<=";
    case Op::Gt: return ">";
    case Op::Ge: return ">=";
  }
  return "?";
}

// ---- command line ----------------------------------------------------------

struct TestCase {
  TestFunction function = nullptr;
  const char* name = "";
  const char* file = "";
  int line = 0;
  std::size_t index = 0;
};

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

enum class Order { File, Name, Random };

struct Options {
  std::vector<std::string> include;
  std::vector<std::string> exclude;
  std::vector<std::string> files;
  std::vector<std::string> files_exclude;
  std::vector<std::string> exact;
  bool list = false;
  bool count = false;
  bool success = false;
  bool durations = false;
  bool absolute_paths = false;
  bool no_summary = false;
  bool help = false;
  int colors = -1;  ///< -1: when stdout is a terminal
  std::uint64_t abort_after = 0;
  Order order = Order::File;
  std::uint64_t seed = 0;
  bool seed_given = false;
};

/// Splits a comma-separated pattern list; `\,` is a literal comma.
std::vector<std::string> split_patterns(std::string_view filter) {
  std::vector<std::string> patterns;
  std::string current;
  for (std::size_t i = 0; i < filter.size(); ++i) {
    if (filter[i] == '\\' && i + 1 < filter.size() && filter[i + 1] == ',') {
      current += ',';
      ++i;
    } else if (filter[i] == ',') {
      if (!current.empty()) patterns.push_back(std::move(current));
      current.clear();
    } else {
      current += filter[i];
    }
  }
  if (!current.empty()) patterns.push_back(std::move(current));
  return patterns;
}

bool matches_any(const std::vector<std::string>& patterns, std::string_view text) noexcept {
  return std::any_of(patterns.begin(), patterns.end(),
                     [&](const std::string& pattern) { return wildcard_match(pattern, text); });
}

bool parse_flag(std::string_view value, bool& flag) noexcept {
  if (value.empty() || value == "true" || value == "1" || value == "yes" || value == "on") {
    flag = true;
  } else if (value == "false" || value == "0" || value == "no" || value == "off") {
    flag = false;
  } else {
    return false;
  }
  return true;
}

bool parse_unsigned(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

/// False (with `error` set) for anything that is not a known option.
bool parse_arguments(int argc, const char* const* argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i] != nullptr ? argv[i] : "";
    const std::size_t equals = argument.find('=');
    const std::string_view key = argument.substr(0, equals);
    const bool has_value = equals != std::string_view::npos;
    const std::string_view value = has_value ? argument.substr(equals + 1) : std::string_view();
    const auto is = [&](std::string_view long_name, std::string_view short_name = {}) {
      return key == long_name || (!short_name.empty() && key == short_name);
    };
    const auto append = [&](std::vector<std::string>& list) {
      for (std::string& pattern : split_patterns(value)) list.push_back(std::move(pattern));
    };
    const auto flag = [&](bool& target) {
      if (parse_flag(value, target)) return true;
      error = "option '" + std::string(key) + "' takes true or false, not '" + std::string(value) + "'";
      return false;
    };
    const auto needs_value = [&] {
      if (has_value) return true;
      error = "option '" + std::string(key) + "' needs a value: " + std::string(key) + "=<value>";
      return false;
    };

    if (is("--test-case", "-tc")) {
      if (!needs_value()) return false;
      append(options.include);
    } else if (is("--test-case-exclude", "-tce")) {
      if (!needs_value()) return false;
      append(options.exclude);
    } else if (is("--source-file", "-sf")) {
      if (!needs_value()) return false;
      append(options.files);
    } else if (is("--source-file-exclude", "-sfe")) {
      if (!needs_value()) return false;
      append(options.files_exclude);
    } else if (is("--test-case-exact")) {
      if (!needs_value()) return false;
      options.exact.emplace_back(value);
    } else if (is("--list-test-cases", "-ltc")) {
      if (!flag(options.list)) return false;
    } else if (is("--count", "-c")) {
      if (!flag(options.count)) return false;
    } else if (is("--success", "-s")) {
      if (!flag(options.success)) return false;
    } else if (is("--durations", "-d")) {
      if (!flag(options.durations)) return false;
    } else if (is("--absolute-paths")) {
      if (!flag(options.absolute_paths)) return false;
    } else if (is("--no-summary") || is("--no-header")) {
      if (!flag(options.no_summary)) return false;
    } else if (is("--help", "-h") || is("-?")) {
      if (!flag(options.help)) return false;
    } else if (is("--no-colors", "-nc")) {
      bool off = true;
      if (!flag(off)) return false;
      options.colors = off ? 0 : 1;
    } else if (is("--colors")) {
      bool on = true;
      if (!flag(on)) return false;
      options.colors = on ? 1 : 0;
    } else if (is("--abort-after", "-aa")) {
      if (!parse_unsigned(value, options.abort_after)) {
        error = "option '" + std::string(key) + "' needs a number: " + std::string(key) + "=<n>";
        return false;
      }
    } else if (is("--rand-seed", "-rs")) {
      if (!parse_unsigned(value, options.seed)) {
        error = "option '" + std::string(key) + "' needs a number: " + std::string(key) + "=<n>";
        return false;
      }
      options.seed_given = true;
    } else if (is("--order-by", "-ob")) {
      if (value == "file") {
        options.order = Order::File;
      } else if (value == "name") {
        options.order = Order::Name;
      } else if (value == "rand") {
        options.order = Order::Random;
      } else {
        error = "option '" + std::string(key) + "' takes file, name or rand, not '" + std::string(value) + "'";
        return false;
      }
    } else {
      error = "unknown option '" + std::string(argument) + "'";
      return false;
    }
  }
  return true;
}

constexpr std::string_view kHelp =
    "Usage: " CVT_TEST_PROGRAM_NAME " [options]\n"
    "\n"
    "Selecting test cases (patterns: comma separated, '*' any text, '?' one character,\n"
    "'\\,' a comma; case sensitive; the whole name must match):\n"
    "  -tc=,  --test-case=<patterns>           run only matching test cases\n"
    "  -tce=, --test-case-exclude=<patterns>   skip matching test cases\n"
    "  -sf=,  --source-file=<patterns>         run only test cases from matching files\n"
    "  -sfe=, --source-file-exclude=<patterns> skip test cases from matching files\n"
    "         --test-case-exact=<name>         run the test cases with exactly this name\n"
    "\n"
    "Querying:\n"
    "  -ltc,  --list-test-cases                print the matching names, one per line\n"
    "  -c,    --count                          print the number of matching test cases\n"
    "         --no-summary                     omit the \"[" CVT_TEST_PROGRAM_NAME "]\" lines\n"
    "  -h,    --help                           this text\n"
    "\n"
    "Running:\n"
    "  -s,    --success                        also print passing assertions\n"
    "  -aa=,  --abort-after=<n>                stop after n failed assertions\n"
    "  -d,    --durations                      print the time and assertion count of every test case\n"
    "  -ob=,  --order-by=file|name|rand        order of execution (default: file, then line)\n"
    "  -rs=,  --rand-seed=<n>                  seed for --order-by=rand\n"
    "  -nc,   --no-colors                      never colour the output\n"
    "         --colors                         always colour the output\n"
    "         --absolute-paths                 print file names as the compiler saw them\n"
    "\n"
    "Exit code: 0 on success (also when no test case matched), 1 on failures, 2 on usage errors.\n";

bool selected(const Options& options, std::string_view name, std::string_view file) {
  if (!options.exact.empty() &&
      std::find(options.exact.begin(), options.exact.end(), name) == options.exact.end()) {
    return false;
  }
  if (!options.include.empty() && !matches_any(options.include, name)) return false;
  if (matches_any(options.exclude, name)) return false;
  if (!options.files.empty() || !options.files_exclude.empty()) {
    const std::string path = display_path(file);
    const std::size_t slash = path.rfind('/');
    const std::string_view base = std::string_view(path).substr(slash == std::string::npos ? 0 : slash + 1);
    const auto matches = [&](const std::vector<std::string>& patterns) {
      return matches_any(patterns, path) || matches_any(patterns, base);
    };
    if (!options.files.empty() && !matches(options.files)) return false;
    if (matches(options.files_exclude)) return false;
  }
  return true;
}

bool stdout_is_terminal() noexcept {
#if defined(_WIN32)
  return false;  // the console may not interpret escape sequences; --colors forces them
#else
  if (std::getenv("NO_COLOR") != nullptr) return false;
  const char* term = std::getenv("TERM");
  if (term == nullptr || std::string_view(term) == "dumb") return false;
  return isatty(fileno(stdout)) != 0;
#endif
}

// ---- running one test case -------------------------------------------------

struct TestOutcome {
  std::uint64_t failed = 0;
  std::uint64_t exceptions = 0;
  unsigned paths = 0;
  bool skipped = false;
};

void emit_exception(Run& run, std::string_view what, std::string_view type) {
  const std::lock_guard lock(state_mutex());
  ActiveTest& test = *run.test;
  ++test.pass_exceptions;
  const Style style = style_for(run);
  std::string out;
  append_header(out, run);
  append_location(out, run, test.file, test.line);
  out += style.red;
  out += "ERROR";
  out += style.reset;
  out += ": unexpected exception: ";
  append_indented(out, what, 2);
  out += '\n';
  if (!type.empty()) {
    out += "  exception type: ";
    out += type;
    out += '\n';
  }
  const std::vector<std::string>& context = unwound_context();
  if (!context.empty()) {
    out += "  with context:\n";
    for (auto it = context.rbegin(); it != context.rend(); ++it) {
      out += "    ";
      append_indented(out, *it, 4);
      out += '\n';
    }
  }
  out += '\n';
  write(run, out);
}

std::string exception_type([[maybe_unused]] const std::exception& error) {
#if defined(CVT_TEST_DETAIL_RTTI)
  const char* name = typeid(error).name();
#  if defined(CVT_TEST_DETAIL_DEMANGLE)
  int status = 0;
  char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
  std::string out = status == 0 && demangled != nullptr ? demangled : name;
  std::free(demangled);
#  else
  std::string out = name;
#  endif
  replace_all(out, "std::__1::", "std::");
  replace_all(out, "std::__ndk1::", "std::");
  return out;
#else
  return {};
#endif
}

TestOutcome run_test_case(Run& run, void (*invoke)(void*), void* object, std::string_view name, const char* file,
                          int line) {
  constexpr unsigned kMaxPasses = 1'000'000;
  ActiveTest test;
  test.name = std::string(name);
  test.file = file;
  test.line = line;
  ActiveTest* outer_test = nullptr;
  {
    const std::lock_guard lock(state_mutex());
    outer_test = run.test;
    run.test = &test;
  }
  const bool outer_thread = tl_test_thread;
  tl_test_thread = true;

  bool previous_abandoned = false;
  std::uint64_t passed_before = 0;
  for (unsigned pass = 0;; ++pass) {
    {
      const std::lock_guard lock(state_mutex());
      test.pass = pass;
      test.stack.assign(1, Frame{&test.root, nullptr});
      // After an abandoned path the body is entered again only to look for
      // subcases that follow it; until one is found the pass may be a dead end.
      test.buffering = previous_abandoned;
      test.entered_new = false;
      test.entered_any = false;
      test.abort_marked = false;
      test.buffer.clear();
      test.pass_failed = 0;
      test.pass_exceptions = 0;
      test.path.clear();
      test.header_printed = false;
      test.header_depth = 0;
      passed_before = hot.passed.load(std::memory_order_relaxed);
    }
    unwound_context().clear();

    enum class End { Normal, Abandoned, Skipped } end = End::Normal;
    try {
      invoke(object);
    } catch (const AbortPath&) {
      end = End::Abandoned;
    } catch (const SkipTest&) {
      end = End::Skipped;
    } catch (const std::exception& error) {
      end = End::Abandoned;
      emit_exception(run, error.what(), exception_type(error));
    } catch (...) {
      end = End::Abandoned;
      emit_exception(run, "(not derived from std::exception)", {});
    }

    const std::lock_guard lock(state_mutex());
    test.stack.assign(1, Frame{&test.root, nullptr});
    const bool dead_end = test.buffering && end != End::Skipped;
    if (end == End::Normal) {
      test.root.complete = test.root.children_complete();
    } else if (end == End::Skipped) {
      test.root.complete = true;
      test.skipped = true;
    } else if (!test.abort_marked && (!test.entered_any || dead_end)) {
      test.root.complete = true;  // abandoned outside every subcase: nothing more can be reached
    }
    if (dead_end) {
      hot.passed.store(passed_before, std::memory_order_relaxed);
    } else {
      test.buffering = false;
      raw_write(run, test.buffer);
      test.failed += test.pass_failed;
      test.exceptions += test.pass_exceptions;
      ++test.paths;
    }
    test.buffering = false;
    test.buffer.clear();
    previous_abandoned = end == End::Abandoned;
    if (test.root.complete || run.stop) break;
    if (pass + 1 >= kMaxPasses) {
      test.pass_exceptions = 0;
      emit_exception(run, "the subcases did not run out after 1000000 passes; do their names change between passes?",
                     {});
      test.exceptions += test.pass_exceptions;
      break;
    }
  }

  tl_test_thread = outer_thread;
  unwound_context().clear();
  TestOutcome outcome;
  outcome.failed = test.failed;
  outcome.exceptions = test.exceptions;
  outcome.paths = test.paths;
  outcome.skipped = test.skipped && test.failed == 0 && test.exceptions == 0;
  {
    const std::lock_guard lock(state_mutex());
    run.failed += test.failed;
    run.test = outer_test;
  }
  return outcome;
}

void append_seconds(std::string& out, double seconds) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%9.3f s", seconds);
  out += buffer;
}

}  // namespace

// ---- formatting services ----------------------------------------------------

void append_signed(std::string& out, long long value) { append_number(out, value); }
void append_unsigned(std::string& out, unsigned long long value) { append_number(out, value); }
void append_float(std::string& out, float value) { append_floating(out, value, "f"); }
void append_double(std::string& out, double value) { append_floating(out, value, ""); }
void append_long_double(std::string& out, long double value) { append_floating(out, value, "L"); }

void append_quoted_char(std::string& out, char value) {
  out += '\'';
  const unsigned char byte = static_cast<unsigned char>(value);
  if (byte >= 0x80) {
    out += "\\x";
    append_hex2(out, byte);
  } else {
    append_escaped(out, std::string_view(&value, 1), '\'');
  }
  out += '\'';
}

void append_code_point(std::string& out, char32_t value) {
  out += "U+";
  std::string digits;
  append_number(digits, static_cast<std::uint32_t>(value), 16);
  if (digits.size() < 4) out.append(4 - digits.size(), '0');
  for (const char c : digits) out += static_cast<char>(c >= 'a' ? c - 'a' + 'A' : c);
}

void append_quoted(std::string& out, std::string_view text) {
  if (text.size() <= kLongString) {
    out += '"';
    append_escaped(out, text);
    out += '"';
    return;
  }
  const std::size_t head = utf8_floor(text, kStringEdge);
  const std::size_t tail = utf8_floor(text, text.size() - kStringEdge);
  out += '"';
  append_escaped(out, text.substr(0, head));
  out += "\"...<";
  append_number(out, tail - head);
  out += " bytes omitted>...\"";
  append_escaped(out, text.substr(tail));
  out += "\" (length ";
  append_number(out, text.size());
  out += ')';
}

void append_quoted_wide(std::string& out, const void* data, std::size_t count, std::size_t unit_size) {
  std::string utf8;
  const auto unit = [&](std::size_t i) -> char32_t {
    if (unit_size == 2) {
      char16_t value = 0;
      std::memcpy(&value, static_cast<const char*>(data) + i * 2, 2);
      return value;
    }
    char32_t value = 0;
    std::memcpy(&value, static_cast<const char*>(data) + i * 4, 4);
    return value;
  };
  for (std::size_t i = 0; i < count; ++i) {
    char32_t code_point = unit(i);
    if (unit_size == 2 && code_point >= 0xD800 && code_point <= 0xDBFF && i + 1 < count) {
      const char32_t low = unit(i + 1);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      }
    }
    append_utf8(utf8, code_point);
  }
  append_quoted(out, utf8);
}

void append_pointer(std::string& out, const void* pointer) {
  if (pointer == nullptr) {
    out += "nullptr";
    return;
  }
  out += "0x";
  append_number(out, reinterpret_cast<std::uintptr_t>(pointer), 16);
}

void append_hex_byte(std::string& out, unsigned value) {
  out += "0x";
  append_hex2(out, value & 0xFFu);
}

void append_bytes(std::string& out, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  out += "bytes[";
  append_number(out, size);
  out += "]{";
  const std::size_t shown = std::min(size, kMaxDumpBytes);
  for (std::size_t i = 0; i < shown; ++i) {
    if (i != 0) out += ' ';
    append_hex2(out, bytes[i]);
  }
  if (shown < size) out += " ...";
  out += '}';
}

void append_duration_unit(std::string& out, long long numerator, long long denominator) {
  struct Unit {
    long long numerator;
    long long denominator;
    const char* text;
  };
  constexpr Unit units[] = {{1, 1'000'000'000, " ns"}, {1, 1'000'000, " us"}, {1, 1000, " ms"}, {1, 1, " s"},
                            {60, 1, " min"},           {3600, 1, " h"},       {86400, 1, " d"}};
  for (const Unit& unit : units) {
    if (unit.numerator == numerator && unit.denominator == denominator) {
      out += unit.text;
      return;
    }
  }
  out += " * ";
  append_number(out, numerator);
  out += '/';
  append_number(out, denominator);
  out += " s";
}

void append_type_name(std::string& out, std::string_view signature, std::string_view probe_signature, bool is_empty) {
  // The probe is the same signature for T = double: whatever surrounds
  // "double" there surrounds the type name in `signature`.
  std::string name;
  const std::size_t at = probe_signature.find("double");
  if (at != std::string_view::npos && signature.size() >= probe_signature.size() - 6) {
    const std::size_t suffix = probe_signature.size() - at - 6;
    name = std::string(signature.substr(at, signature.size() - at - suffix));
  }
  for (const std::string_view noise : {"std::__1::", "std::__ndk1::", "std::__cxx11::"}) {
    replace_all(name, noise, "std::");
  }
  for (const std::string_view noise : {"class ", "struct ", "enum ", "union "}) replace_all(name, noise, "");
  if (name.size() > 120) name = name.substr(0, 117) + "...";
  if (name.empty()) {
    out += "{?}";
  } else if (is_empty) {
    out += name;  // a tag such as std::nullopt_t: the type says it all
  } else {
    out += "{? ";
    out += name;
    out += '}';
  }
}

std::string string_difference(std::string_view lhs, std::string_view rhs) {
  if (lhs == rhs || std::max(lhs.size(), rhs.size()) < kDifferenceFrom) return {};
  std::size_t index = 0;
  const std::size_t common = std::min(lhs.size(), rhs.size());
  while (index < common && lhs[index] == rhs[index]) ++index;

  std::string out = "first difference at index ";
  append_number(out, index);
  const std::string_view before = lhs.substr(0, index);
  if (lhs.find('\n') != std::string_view::npos || rhs.find('\n') != std::string_view::npos) {
    const std::size_t line = static_cast<std::size_t>(std::count(before.begin(), before.end(), '\n')) + 1;
    const std::size_t line_start = before.rfind('\n') == std::string_view::npos ? 0 : before.rfind('\n') + 1;
    out += " (line ";
    append_number(out, line);
    out += ", column ";
    append_number(out, index - line_start + 1);
    out += ')';
  }
  out += "; lengths ";
  append_number(out, lhs.size());
  out += " and ";
  append_number(out, rhs.size());

  // Both excerpts start at the same index, so they are identical up to the caret.
  const std::size_t marker = utf8_floor(lhs, index);
  const std::size_t start = utf8_floor(lhs, marker > 24 ? marker - 24 : 0);
  std::size_t caret = 0;
  const auto excerpt = [&](std::string_view label, std::string_view text) {
    out += "\n  ";
    out += label;
    const std::size_t end = utf8_floor(text, std::min(text.size(), index + 40));
    out += start > 0 ? "...\"" : "\"";
    caret = label.size() + (start > 0 ? 4 : 1) + append_escaped(out, text.substr(start, marker - start));
    if (end > marker) append_escaped(out, text.substr(marker, end - marker));
    out += end < text.size() ? "\"..." : "\"";
  };
  excerpt("lhs: ", lhs);
  excerpt("rhs: ", rhs);
  out += "\n  ";
  out.append(caret, ' ');
  out += '^';
  return out;
}

std::string bytes_difference(const void* lhs, std::size_t lhs_size, const void* rhs, std::size_t rhs_size) {
  const auto* a = static_cast<const unsigned char*>(lhs);
  const auto* b = static_cast<const unsigned char*>(rhs);
  std::size_t index = 0;
  const std::size_t common = std::min(lhs_size, rhs_size);
  while (index < common && a[index] == b[index]) ++index;
  if (index == lhs_size && index == rhs_size) return {};

  std::string out = "first difference at index ";
  append_number(out, index);
  out += " (0x";
  append_number(out, index, 16);
  out += "); sizes ";
  append_number(out, lhs_size);
  out += " and ";
  append_number(out, rhs_size);
  const std::size_t start = index > 8 ? index - 8 : 0;
  std::string label = "[";
  append_number(label, start);
  label += "...]: ";
  const auto excerpt = [&](std::string_view side, const unsigned char* bytes, std::size_t size) {
    out += "\n  ";
    out += side;
    out += label;
    const std::size_t end = std::min(size, index + 8);
    for (std::size_t i = start; i < end; ++i) {
      if (i != start) out += ' ';
      append_hex2(out, bytes[i]);
    }
    if (end <= index) out += start < end ? " (ends)" : "(ends)";
  };
  excerpt("lhs", a, lhs_size);
  excerpt("rhs", b, rhs_size);
  out += "\n  ";
  out.append(3 + label.size() + (index - start) * 3, ' ');
  out += "^^";
  return out;
}

struct StreamCapture::State {
  struct Buffer final : std::streambuf {
    std::string text;
    int_type overflow(int_type c) override {
      if (!traits_type::eq_int_type(c, traits_type::eof())) text += traits_type::to_char_type(c);
      return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char* data, std::streamsize count) override {
      text.append(data, static_cast<std::size_t>(count));
      return count;
    }
  };
  Buffer buffer;
  std::ostream stream{&buffer};
};

StreamCapture::StreamCapture() : state_(new State) {}
StreamCapture::~StreamCapture() { delete state_; }
std::ostream& StreamCapture::stream() noexcept { return state_->stream; }
std::string_view StreamCapture::text() const noexcept { return state_->buffer.text; }

// ---- reporting ---------------------------------------------------------------

void report_assertion(const Site& site, unsigned flags, bool passed, std::string_view expansion, std::string_view note,
                      LazyText detail) {
  Run* run = nullptr;
  bool stop = false;
  {
    const std::lock_guard lock(state_mutex());
    run = g_run;
    if (passed) {
      hot.passed.fetch_add(1, std::memory_order_relaxed);
      if (run != nullptr && run->success) {
        emit_assertion(*run, site.file, site.line, Verdict::Passed, assertion_headline(site), expansion, {}, {});
      }
      return;
    }
    if (run == nullptr) {
      emit_orphan(site.file, site.line, assertion_headline(site));
    } else {
      stop = count_failure(*run);
      emit_assertion(*run, site.file, site.line, Verdict::Failed, assertion_headline(site), expansion, note, detail);
    }
  }
  leave_path(run, (flags & kFatal) != 0u, stop, "a REQUIRE failed");
}

void report_comparison(const Site& site, unsigned flags, bool passed, std::string_view lhs, Op op, std::string_view rhs,
                       std::string_view note, LazyText detail) {
  std::string expansion;
  const bool one_line = lhs.size() + rhs.size() <= kInlineExpansion;
  expansion += lhs;
  expansion += one_line ? ' ' : '\n';
  expansion += op_text(op);
  expansion += one_line ? ' ' : '\n';
  expansion += rhs;
  report_assertion(site, flags, passed, expansion, note, detail);
}

void report_approx(const Site& site, unsigned flags, bool passed, double lhs, double rhs, double tolerance) {
  std::string expansion;
  append_double(expansion, lhs);
  expansion += passed ? " ~= " : " !~= ";
  append_double(expansion, rhs);
  expansion += " (difference ";
  append_double(expansion, lhs > rhs ? lhs - rhs : rhs - lhs);
  expansion += ", tolerance ";
  append_double(expansion, tolerance);
  expansion += ')';
  report_assertion(site, flags, passed, expansion, {}, {});
}

void fail_soft(const char* file, int line, std::string_view text) {
  Run* run = nullptr;
  bool stop = false;
  {
    const std::lock_guard lock(state_mutex());
    run = g_run;
    if (run == nullptr) {
      emit_orphan(file, line, text);
    } else {
      stop = count_failure(*run);
      emit_assertion(*run, file, line, Verdict::Failed, text.empty() ? std::string_view("explicit failure") : text, {},
                     {}, {});
    }
  }
  leave_path(run, false, stop, {});
}

void fail_fatal(const char* file, int line, std::string_view text) {
  Run* run = nullptr;
  {
    const std::lock_guard lock(state_mutex());
    run = g_run;
    if (run == nullptr) {
      emit_orphan(file, line, text);
    } else {
      count_failure(*run);
      emit_assertion(*run, file, line, Verdict::Failed, text.empty() ? std::string_view("explicit failure") : text, {},
                     {}, {});
    }
  }
  if (tl_test_thread) throw AbortPath{};
  const std::lock_guard lock(state_mutex());
  abort_off_thread(run, "FAIL was reached");
}

void message(const char* file, int line, std::string_view text) {
  const std::lock_guard lock(state_mutex());
  Run* run = g_run;
  if (run == nullptr) return;
  const Style style = style_for(*run);
  std::string out;
  append_header(out, *run);
  append_location(out, *run, file, line);
  out += style.cyan;
  out += "MESSAGE";
  out += style.reset;
  out += ": ";
  append_indented(out, text, 2);
  out += '\n';
  write(*run, out);
}

void skip(const char* file, int line, std::string_view reason) {
  Run* run = nullptr;
  {
    const std::lock_guard lock(state_mutex());
    run = g_run;
    if (run != nullptr) {
      const Style style = style_for(*run);
      std::string out;
      append_header(out, *run);
      append_location(out, *run, file, line);
      out += style.yellow;
      out += "SKIPPED";
      out += style.reset;
      out += ": ";
      append_indented(out, reason.empty() ? std::string_view("no reason given") : reason, 2);
      out += '\n';
      write(*run, out);
    }
  }
  if (tl_test_thread) throw SkipTest{};
  const std::lock_guard lock(state_mutex());
  abort_off_thread(run, "SKIP was reached");
}

void info_push(InfoNode& node) noexcept {
  node.uncaught = std::uncaught_exceptions();
  node.previous = tl_info_head;
  tl_info_head = &node;
  if (tl_has_unwound_context && node.uncaught == 0) {
    // The exception that left earlier scopes was caught by the test itself.
    tl_has_unwound_context = false;
    unwound_context().clear();
  }
}

void info_pop(InfoNode& node) noexcept {
  if (std::uncaught_exceptions() > node.uncaught) {
    // An exception is passing through this scope: keep the text for its report.
    try {
      std::string text;
      node.render(node, text);
      unwound_context().push_back(std::move(text));
      tl_has_unwound_context = true;
    } catch (...) {  // NOLINT(bugprone-empty-catch): a note is not worth terminating for
    }
  }
  tl_info_head = node.previous;
}

// ---- test cases and subcases ---------------------------------------------------

int register_test_case(TestFunction function, const char* name, const char* file, int line) noexcept {
  std::vector<TestCase>& cases = registry();
  cases.push_back(TestCase{function, name, file, line, cases.size()});
  return 0;
}

Subcase::Subcase(std::string_view name, const char* file, int line) : uncaught_(std::uncaught_exceptions()) {
  const std::lock_guard lock(state_mutex());
  if (g_run == nullptr || g_run->test == nullptr || !tl_test_thread) return;
  ActiveTest& test = *g_run->test;
  Frame& frame = test.stack.back();
  Node* child = nullptr;
  for (const auto& candidate : frame.node->children) {
    if (candidate->line == line && candidate->name == name && std::string_view(candidate->file) == file) {
      child = candidate.get();
      break;
    }
  }
  if (child == nullptr) {
    frame.node->children.push_back(std::make_unique<Node>());
    child = frame.node->children.back().get();
    child->name = std::string(name);
    child->file = file;
    child->line = line;
  }
  // One child per level and pass; the same subcase may be entered again by a loop.
  if (frame.entered_child != child && (child->complete || frame.entered_child != nullptr)) return;
  frame.entered_child = child;
  entered_ = true;
  test.entered_any = true;
  if (!child->entered_before) {
    child->entered_before = true;
    test.entered_new = true;
    if (test.buffering) {
      // The pass leads somewhere new after all: what it reported so far stands.
      test.buffering = false;
      raw_write(*g_run, test.buffer);
      test.buffer.clear();
    }
  }
  test.stack.push_back(Frame{child, nullptr});
  if (test.stack.size() - 1 > test.path.size()) test.path.push_back(child);
}

Subcase::~Subcase() {
  if (!entered_) return;
  const std::lock_guard lock(state_mutex());
  if (g_run == nullptr || g_run->test == nullptr) return;
  ActiveTest& test = *g_run->test;
  if (test.stack.size() < 2) return;
  Node* node = test.stack.back().node;
  if (std::uncaught_exceptions() > uncaught_) {
    // Abandoned by a failed REQUIRE or an exception. The innermost subcase
    // counts as done; whether its parents have more to run is found out by
    // entering the body again.
    if (!test.abort_marked) {
      test.abort_marked = true;
      node->complete = true;
    }
  } else {
    node->complete = node->children_complete();
  }
  test.stack.pop_back();
}

IsolatedResult run_isolated_impl(void (*invoke)(void*), void* object, const IsolatedOptions& options,
                                 std::string_view name, const char* file, int line) {
  Run isolated;
  isolated.success = options.success;
  isolated.absolute_paths = options.absolute_paths;
  isolated.abort_after = options.abort_after;

  Run* outer_run = nullptr;
  {
    const std::lock_guard lock(state_mutex());
    outer_run = g_run;
    g_run = &isolated;
  }
  const bool outer_verbose = hot.verbose.exchange(options.success);
  const std::uint64_t outer_passed = hot.passed.exchange(0);
  InfoNode* const outer_info = tl_info_head;
  tl_info_head = nullptr;
  std::vector<std::string> outer_context = std::move(unwound_context());
  unwound_context().clear();

  const TestOutcome outcome = run_test_case(isolated, invoke, object, name, file, line);

  IsolatedResult result;
  result.passed = hot.passed.load();
  result.failed = outcome.failed;
  result.exceptions = outcome.exceptions;
  result.paths = outcome.paths;
  result.skipped = outcome.skipped;

  unwound_context() = std::move(outer_context);
  tl_info_head = outer_info;
  hot.passed.store(outer_passed);
  hot.verbose.store(outer_verbose);
  {
    const std::lock_guard lock(state_mutex());
    g_run = outer_run;
  }
  result.output = std::move(isolated.captured);
  return result;
}

// ---- filters and paths -----------------------------------------------------------

bool wildcard_match(std::string_view pattern, std::string_view text) noexcept {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;
  std::size_t star_text = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      star_text = t;
    } else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++star_text;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

bool filter_matches(std::string_view filter, std::string_view text) {
  return matches_any(split_patterns(filter), text);
}

Selection selects(std::initializer_list<std::string_view> arguments, std::string_view name, std::string_view file) {
  std::vector<std::string> storage(arguments.begin(), arguments.end());
  std::vector<const char*> argv = {CVT_TEST_PROGRAM_NAME};
  for (const std::string& argument : storage) argv.push_back(argument.c_str());
  Options options;
  std::string error;
  if (!parse_arguments(static_cast<int>(argv.size()), argv.data(), options, error)) return Selection::UsageError;
  return selected(options, name, file) ? Selection::Selected : Selection::NotSelected;
}

std::string display_path(std::string_view file) {
  std::string path(file);
  std::replace(path.begin(), path.end(), '\\', '/');
#if defined(CVT_TEST_SOURCE_ROOT)
  std::string root = CVT_TEST_SOURCE_ROOT;
  std::replace(root.begin(), root.end(), '\\', '/');
  while (!root.empty() && root.back() == '/') root.pop_back();
  if (!root.empty() && path.size() > root.size() + 1 && path.compare(0, root.size(), root) == 0 &&
      path[root.size()] == '/') {
    return path.substr(root.size() + 1);
  }
#endif
  const std::size_t at = path.rfind("/core/");
  return at == std::string::npos ? path : path.substr(at + 1);
}

}  // namespace cvt::test::fw

// ---- the runner ---------------------------------------------------------------------

namespace cvt::test {

int run(int argc, const char* const* argv) {
  using namespace fw;

  Options options;
  std::string error;
  if (!parse_arguments(argc, argv, options, error)) {
    std::fprintf(stderr, "%.*serror: %s\n%.*srun with --help for the list of options\n", static_cast<int>(kTag.size()),
                 kTag.data(), error.c_str(), static_cast<int>(kTag.size()), kTag.data());
    return 2;
  }
  if (options.help) {
    std::fwrite(kHelp.data(), 1, kHelp.size(), stdout);
    return 0;
  }

  Run session;
  session.file = stdout;
  session.success = options.success;
  session.absolute_paths = options.absolute_paths;
  session.colors = options.colors < 0 ? stdout_is_terminal() : options.colors != 0;
  session.abort_after = options.abort_after;
  const Style style = style_for(session);
  const auto line = [&](std::string_view text) {
    if (options.no_summary) return;
    std::string out(kTag);
    out += text;
    out += '\n';
    raw_write(session, out);
  };

  // Select and order.
  struct Selected {
    TestCase test;
    std::string path;
    double seconds = 0.0;
    std::uint64_t assertions = 0;
  };
  std::vector<Selected> tests;
  const std::size_t registered = registry().size();
  for (const TestCase& test : registry()) {
    if (selected(options, test.name, test.file)) tests.push_back(Selected{test, display_path(test.file), 0.0, 0});
  }
  if (options.order == Order::Name) {
    std::stable_sort(tests.begin(), tests.end(), [](const Selected& a, const Selected& b) {
      const int names = std::strcmp(a.test.name, b.test.name);
      if (names != 0) return names < 0;
      if (a.path != b.path) return a.path < b.path;
      return a.test.line < b.test.line;
    });
  } else {
    std::stable_sort(tests.begin(), tests.end(), [](const Selected& a, const Selected& b) {
      if (a.path != b.path) return a.path < b.path;
      if (a.test.line != b.test.line) return a.test.line < b.test.line;
      return a.test.index < b.test.index;
    });
  }
  if (options.order == Order::Random) {
    if (!options.seed_given) {
      const auto now = std::chrono::system_clock::now().time_since_epoch().count();
      options.seed = static_cast<std::uint64_t>(now) % 1'000'000;
    }
    // Own generator and shuffle: the order for a seed is the same on every platform.
    std::uint64_t state = options.seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
    const auto next = [&state] {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return state;
    };
    for (std::size_t i = tests.size(); i > 1; --i) std::swap(tests[i - 1], tests[next() % i]);
  }

  if (options.list) {
    std::string out;
    for (const Selected& entry : tests) {
      out += entry.test.name;
      out += '\n';
    }
    raw_write(session, out);
    std::string summary = "test cases matching the filters: ";
    append_unsigned(summary, tests.size());
    line(summary);
    return 0;
  }
  if (options.count) {
    std::string out;
    append_unsigned(out, tests.size());
    if (options.no_summary) {
      out += '\n';
      raw_write(session, out);
    } else {
      line("test cases matching the filters: " + out);
    }
    return 0;
  }
  if (tests.empty()) {
    std::string out = "no test case matches the filters (";
    append_unsigned(out, registered);
    out += " registered); nothing was run";
    line(out);
    line("Status: SUCCESS");
    return 0;
  }
  if (options.order == Order::Random) {
    std::string out = "random order, --rand-seed=";
    append_unsigned(out, options.seed);
    line(out);
  }

  // Run.
  {
    const std::lock_guard lock(state_mutex());
    g_run = &session;
  }
  hot.passed.store(0);
  hot.verbose.store(options.success);
  std::uint64_t cases_run = 0;
  std::uint64_t cases_failed = 0;
  std::uint64_t cases_skipped = 0;
  std::vector<const Selected*> failures;
  const auto started = std::chrono::steady_clock::now();
  for (Selected& entry : tests) {
    TestFunction function = entry.test.function;
    const std::uint64_t passed_before = hot.passed.load();
    const auto test_started = std::chrono::steady_clock::now();
    const TestOutcome outcome = run_test_case(
        session, [](void* opaque) { (*static_cast<TestFunction*>(opaque))(); }, &function, entry.test.name,
        entry.test.file, entry.test.line);
    entry.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - test_started).count();
    entry.assertions = hot.passed.load() - passed_before + outcome.failed;
    ++cases_run;
    if (outcome.skipped) {
      ++cases_skipped;
    } else if (outcome.failed != 0 || outcome.exceptions != 0) {
      ++cases_failed;
      failures.push_back(&entry);
    }
    const std::lock_guard lock(state_mutex());
    if (session.stop) break;
  }
  const double total_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  hot.verbose.store(false);
  std::uint64_t assertions_failed = 0;
  {
    const std::lock_guard lock(state_mutex());
    g_run = nullptr;
    assertions_failed = session.failed;
  }
  const std::uint64_t assertions_passed = hot.passed.load();

  // Report.
  if (session.stop) {
    std::string out = "stopped after ";
    append_unsigned(out, assertions_failed);
    out += " failed assertions (--abort-after); ";
    append_unsigned(out, tests.size() - cases_run);
    out += " test cases were not run";
    line(out);
  }
  if (!failures.empty()) {
    line("failed test cases:");
    for (const Selected* entry : failures) {
      std::string out = "  ";
      out += session.absolute_paths ? std::string(entry->test.file) : entry->path;
      out += ':';
      append_signed(out, entry->test.line);
      out += ": ";
      out += entry->test.name;
      line(out);
    }
  }
  if (options.durations || total_seconds > 1.0) {
    std::vector<const Selected*> by_time;
    for (const Selected& entry : tests) by_time.push_back(&entry);
    std::stable_sort(by_time.begin(), by_time.end(),
                     [](const Selected* a, const Selected* b) { return a->seconds > b->seconds; });
    if (!options.durations && by_time.size() > 5) by_time.resize(5);
    line(options.durations ? "durations:" : "slowest test cases:");
    for (const Selected* entry : by_time) {
      std::string out;
      append_seconds(out, entry->seconds);
      if (options.durations) {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%10llu assertions", static_cast<unsigned long long>(entry->assertions));
        out += buffer;
      }
      out += "  ";
      out += entry->test.name;
      line(out);
    }
  }
  {
    std::string out = "test cases: ";
    append_unsigned(out, cases_run);
    out += " | ";
    append_unsigned(out, cases_run - cases_failed - cases_skipped);
    out += " passed | ";
    append_unsigned(out, cases_failed);
    out += " failed | ";
    append_unsigned(out, cases_skipped);
    out += " skipped";
    line(out);
    out = "assertions: ";
    append_unsigned(out, assertions_passed + assertions_failed);
    out += " | ";
    append_unsigned(out, assertions_passed);
    out += " passed | ";
    append_unsigned(out, assertions_failed);
    out += " failed";
    line(out);
  }
  const bool success = cases_failed == 0 && assertions_failed == 0;
  std::string status = "Status: ";
  status += success ? style.green : style.red;
  status += success ? "SUCCESS" : "FAILURE";
  status += style.reset;
  line(status);
  return success ? 0 : 1;
}

}  // namespace cvt::test

#if defined(CVT_TEST_MAIN)
int main(int argc, char** argv) { return ::cvt::test::run(argc, argv); }
#endif

#endif  // implementation
