---
name: add-format
description: Add support for reading and/or writing one file format inside an existing format family of the Convertible core (reader, writer, options, registration, tests, real-file fixtures with SOURCES.md, generated docs). Use when a format id should become readable, writable or detectable, or when an existing format gains a codec.
---

# Add a format to an existing family

Worked example to copy from: `core/README.md`, section "Adding a format" (binary PPM in the
`image` family). Rules for tests: `core/tests/README.md`. Fixtures: the `make-fixtures` skill.

## Steps

1. **Pick the family** that owns the format (`core/README.md`, "Who owns what"). Work only in
   `core/src/families/<family>/`, `core/tests/families/<family>/` and
   `core/tests/data/<family>/`. Check the format is not registered already:
   `build/dev/core/convertible formats --json | grep '"id": "<id>"'`.
2. **Write the codec** in `core/src/families/<family>/<format>.cpp`:
   - `FormatInfo`: lower-case `id`, `name`, `description`, `category`, extensions without dots
     (first one is the default for output), `mime`, `popularity` 0-100, `signatures`
     (`Signature::at(offset, "89 'PNG' 0D 0A")`) and/or a `sniff` returning 0-100 (>= 90 only
     when certain). Never claim a shared extension (`txt`, `xml`) without a content test.
   - `Codec` with `kind`, `read` and/or `write`, `lossy` where information is discarded.
   - Read defensively with `util::ByteReader`; call `ctx.limits.check_*` before allocating
     anything sized by the input; `CVT_TRY(ctx.checkpoint(fraction))` in long loops; return
     `Status::Decode` / `Status::Limit` with a message a user can act on.
   - Options: standard ones from `engine/standard_options.hpp` (`quality`, `preset`,
     `compression_level`, `strip_metadata`, ...) before inventing new keys; reader option keys
     must not collide with writer keys.
   - Writers: honour `strip_metadata` (default true), never write dates, host or user names.
3. **Register** it from `register_<family>_family()` in `<family>_family.cpp` (declare the
   function in the family's private header). No CMake edit: sources are globbed.
4. **Fixtures**: files made by independent software, tiny, synthetic, scrubbed, each with an
   entry in `core/tests/data/<family>/SOURCES.md` (tool, version, exact command).
5. **Tests** in `core/tests/families/<family>/<format>_test.cpp`, named
   `"<family>/<format>: ..."`: decode fixtures against known content, round trips with metric
   thresholds per quality (lower quality must be smaller), hostile input (`test::fuzz_reader`,
   truncations, bombs), every option, detection and `test::convert` end to end.
6. **Build and test**:
   ```sh
   cmake --preset dev && cmake --build --preset dev -j
   build/dev/core/convertible_tests --test-case="<family>/<format>*"
   ctest --preset dev
   cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
   ```
7. **Regenerate the format list** and check the rest of the repository:
   ```sh
   tools/gen-formats-doc
   tools/privacy-audit
   ```
   Commit `docs/FORMATS.md` and the README table together with the code.
8. For core changes also run the Linux rehearsal (`verify-linux` skill):
   `tools/ci/docker-linux.sh gcc` and `tools/ci/docker-linux.sh clang`.
