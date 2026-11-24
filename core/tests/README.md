# Tests of the Convertible core

Everything below this directory is compiled into one executable,
`convertible_tests`, and run by CTest together with an end-to-end smoke test
of the command-line tool (`cli_smoke.cmake`).

```sh
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan    # before every hand-in

build/dev/core/convertible_tests --test-case="image/png*"         # by name (wildcards, comma lists)
build/dev/core/convertible_tests --source-file="*families/image*" # by file
build/dev/core/convertible_tests --list-test-cases
build/dev/core/convertible_tests --durations                      # where the time goes
```

## Layout

```
tests/
  main.cpp                  the runner (nothing to edit)
  support/test.hpp          the test framework: TEST_CASE, CHECK, REQUIRE, ...   (its header comment is the manual)
  support/test_support.hpp  engine helpers: convert, read_as<T>, write_from, fuzz_reader, random_bytes
  support/fixtures.hpp      fixture("family/name") -> bytes of tests/data/family/name
  support/metrics.hpp       PSNR, SSIM, audio SNR, text similarity, data/table/mesh comparison, generators
  engine/  util/  codec/    tests of the shared code
  families/<family>/        tests of one format family (see families/README.md)
  data/<family>/            fixtures + SOURCES.md (see data/README.md)
  cli_smoke.cmake           end-to-end run of the CLI
```

New `*.cpp` files are picked up automatically; no CMake file needs editing.

## Writing a test

```cpp
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

TEST_CASE("image/png: decodes a reference file") {
  const auto image = test::read_as<ir::Image>("png", test::fixture("image/gradient_8x8.png"));
  REQUIRE_OK(image);                                   // prints the error code and message on failure
  CHECK(image->width() == 8);
  CHECK(test::pixels_equal(image->frames[0], test::gradient_frame(8, 8)));
}

TEST_CASE("image/png: damaged input is an error") {
  const Bytes valid = test::fixture("image/gradient_8x8.png");
  CHECK_STATUS(test::read_as<ir::Image>("png", ByteView(valid).first(20)), Status::Decode);
  test::fuzz_reader<ir::Image>("png", valid);           // every truncation and a set of corruptions
}
```

- Name test cases `"<family>/<format>: what is checked"` (shared code:
  `"zip: ..."`, `"planner: ..."`), so they can be selected with
  `--test-case="image/png*"`.
- The complete macro list, the rules for `SUBCASE`, value printing and the
  runner options are documented at the top of `support/test.hpp`. The
  essentials: `CHECK` continues, `REQUIRE` abandons the test case; a failing
  comparison prints both operands; `CHECK_OK(result)`,
  `CHECK_STATUS(result, Status::X)` and `CHECK_APPROX(a, b, tolerance)` cover
  the common engine cases; `INFO(...)`/`CAPTURE(x)` add context inside loops.
- Tests are deterministic: no clock, no random device, no network, no files
  outside a `util::TempDir`. Pseudo-random input comes from
  `test::random_bytes(n, seed)`.
- Byte strings with NULs need the `sv` suffix (`"\x00\x01"sv`); a plain
  literal would stop at the first NUL.
- Tests must be fast: the whole suite runs in seconds in a Debug build. Keep
  generated inputs small and loops bounded.

## Helpers at a glance (`support/test_support.hpp`)

| Call | Purpose |
| --- | --- |
| `test::convert(input, source, target, options_json, filename, limits)` | full conversion through the shared registry, exactly as `cvt_convert` does; `source` empty = detect; returns status, message, data, format, warnings |
| `test::read_as<ir::T>(format, bytes, options_json, &warnings, limits)` | run the registered reader of a format for one IR type |
| `test::write_from(format, ir_value, options_json, &warnings, limits)` | run the registered writer (move-only IRs are moved in) |
| `test::fuzz_reader<ir::T>(format, valid_bytes)` | truncations + deterministic corruptions; must simply return |
| `test::random_bytes(n, seed)`, `test::all_byte_values()`, `test::bytes("...")`, `test::str(bytes)` | data helpers |

Quality metrics (`support/metrics.hpp`) return NaN when the two sides are not
comparable (different size, rate, format), so a wrong decode can never pass a
threshold by accident. Starting points for thresholds are in the header.

## What every format must have

1. **Decode tests on real files.** Fixtures written by independent, widely
   used software (never by our own encoder), checked against known content:
   exact pixels or samples for lossless formats, a PSNR/SNR threshold against
   the known pattern for lossy ones, exact text and structure for documents
   and data. Cover the variants that exist in the wild (bit depths, colour
   types, interlacing, byte orders, compression modes, old and new versions).
2. **Encode → decode round trips.** Lossless formats must round-trip exactly
   (`pixels_equal`, `tables_equal`, `data_equal`, …). Lossy formats must meet
   a metric threshold at each of several `quality` values, and lower quality
   must produce smaller output. Where an independent decoder is available on
   developer machines, record in the family's `SOURCES.md` how our output was
   verified with it.
3. **Hostile input.** Truncated and corrupt files return `Status::Decode` or
   `Status::Limit` and never crash, hang or allocate without bound
   (`fuzz_reader`, hand-made bombs against `Limits`, tests run under the
   `asan` preset).
4. **Options.** Every declared option changes the output as documented;
   out-of-range values are clamped by the engine; invalid free-text values
   are `Status::InvalidArgument`.
5. **Integration.** The format is detected from content (and not confused
   with its neighbours), appears in `list_targets` where expected, converts
   through `test::convert` end to end, and `Registry::instance().validate()`
   stays empty.
