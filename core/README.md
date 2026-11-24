# Convertible core

The single C++ engine behind every Convertible app (iOS, Android, Web/WASM,
Windows, Linux, macOS) and the `convertible` command-line tool. It detects
file formats and converts between them, entirely offline: no network access,
no telemetry, no other programs started, nothing read or written except the
buffers and paths the caller passes in.

Applications use one header, [`include/convertible/convertible.h`](include/convertible/convertible.h)
(plain C; `convertible.hpp` is a thin RAII wrapper over it). Everything under
`src/` is internal.

Contents: [Build and test](#build-and-test) · [Layout](#layout) ·
[Architecture](#architecture) · [Rules](#rules) · [Who owns what](#who-owns-what) ·
[Adding a format](#adding-a-format-a-complete-example) · [Bridges](#bridges) ·
[Options](#options) · [Audio and video codecs](#audio-and-video-codecs) ·
[Utilities](#utilities) · [Tests](#tests) · [Imported code](#imported-code) ·
[Notes for app developers](#notes-for-app-developers)

## Build and test

Requirements: CMake 3.22 or newer and a C++23 compiler (Apple Clang 21,
Clang 18 with libc++ 18, GCC 14, MSVC 2022 17.10, current Emscripten). The
build needs nothing else and works offline from a clean checkout.

```sh
cmake --preset dev                 # Debug + tests + CLI, in build/dev
cmake --build --preset dev -j
ctest --preset dev                 # unit tests and the CLI smoke test

cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan   # AddressSanitizer + UBSan
cmake --preset release && cmake --build --preset release -j                    # optimised CLI
```

Run a subset while developing (see `tests/README.md` for all options):

```sh
build/dev/core/convertible_tests --test-case="zip*,deflate*"
build/dev/core/convertible_tests --source-file="*families/encoding*"
```

CMake options: `CVT_BUILD_TESTS`, `CVT_BUILD_CLI` (both default ON when the
repository is the top-level project, OFF when the core is pulled into an app
build with `add_subdirectory(core)`), `CVT_SANITIZE` (e.g. `address;undefined`),
`CVT_WASM_EXCEPTIONS` (Emscripten only, default ON). The language level is
C++26 where the compiler has it and C++23 otherwise; pass
`-DCMAKE_CXX_STANDARD=23` to check the baseline explicitly. Our own code must
compile without warnings under `-Wall -Wextra -Wpedantic` and MSVC `/W4`.

The CLI:

```sh
convertible convert notes.txt -t base64          # writes notes.b64
convertible convert notes.b64 -t bin -o copy.txt
convertible convert in.txt -t txt --opt encoding=utf-16le --opt bom=true -o out.txt
convertible convert in.txt -t base64 --list-options
convertible detect file            convertible targets file-or-format [--json]
convertible formats [--json] [--category image]
```

## Layout

```
core/
  CMakeLists.txt            target convertible_core (static; alias convertible::core)
  cmake/CvtHelpers.cmake    warnings, sanitizers, cvt_add_imported()
  include/convertible/      convertible.h (C ABI, the only header apps use), convertible.hpp
  src/engine/               errors, limits, options, standard_options, progress, context,
                            registry, planner, detect, convert, known_formats, capi
  src/ir/                   one header per intermediate representation + kind.hpp, any.hpp
  src/codec/                audio/video decoder and encoder interfaces: ids.hpp, codec.hpp,
                            registry.hpp, colorspace.hpp
  src/util/                 bytes, text, base64, hex, crc, hash, deflate, zip, json, xml, fs, mapped_file
  src/families/<family>/    one directory per format family (see "Who owns what")
  src/families/families.hpp, register_all.cpp
  src/imported/<name>/      imported open-source components, where policy allows (see "Imported code")
  cli/main.cpp              the `convertible` tool
  tests/                    engine/, util/, codec/, families/<family>/, support/, data/<family>/
```

Sources below `src/engine`, `src/codec`, `src/ir`, `src/util`, `src/families`
and `tests` are picked up by glob: adding a file never requires touching a
CMake file. Includes are written relative to `src/` (`"engine/registry.hpp"`,
`"ir/image.hpp"`, `"util/bytes.hpp"`, `"families/image/png.hpp"`) and, in
tests, relative to `tests/` (`"support/test_support.hpp"`).

## Architecture

**Formats** (`FormatInfo`, `engine/registry.hpp`) describe a file type: id,
name, category, extensions, MIME type, popularity, magic-number signatures and
an optional `sniff` function. A format can exist without any codec; then it
can be detected and still converts to the universal targets below.

**Intermediate representations** (`src/ir/`, `enum class Kind`). Codecs never
convert format to format; they read a format into an IR value or write one
from it:

| Kind | Type | Content |
| --- | --- | --- |
| `Blob` | `ir::Blob` | opaque bytes + file name |
| `Image` | `ir::Image` | RGBA8 frames (straight alpha), optional HDR floats, dpi, metadata |
| `Audio` | `ir::Audio` | interleaved float32 PCM, tags |
| `Video` | `ir::Video` | pull-based stream of frames, audio chunks and compressed packets (move-only) |
| `Document` | `ir::Document` | rich-text tree: headings, paragraphs, lists, tables, images, inlines |
| `PlainText` | `ir::PlainText` | UTF-8 text |
| `Data` | `ir::Data` | JSON-shaped tree (`cvt::Json`) |
| `Table` | `ir::Table` | workbook of sheets of typed cells |
| `Archive` | `ir::Archive` | named entries with sanitised paths |
| `Subtitles`, `Mesh`, `Font`, `Geo`, `Calendar`, `Contacts` | `ir::…` | see the headers |

`AnyIR` (`ir/any.hpp`) holds one value of any kind. It is **move-only**
(because `ir::Video` is a stream): pass it by value and `std::move` it.

**Codecs** (`Codec`) read and/or write one format as one kind. A format may
have several codecs (html: `Document` reader and writer plus a `Table`
writer). **Bridges** (`Bridge`) convert between kinds (`Table → Document`,
`Document → PlainText`, `Blob → Archive`).

**Planner** (`engine/planner.hpp`). For a (source, target) pair it picks the
cheapest route `reader → 0..2 bridges → writer` by summed cost, with
deterministic tie-breaks (fewer bridges, fewer lossy steps, earlier
registration). Cost guideline: 10 for a fast faithful codec, 20–50 for slow or
approximate ones, 5–30 for bridges. Two special rules:

- *Every format is implicitly readable as `Blob`* (the raw file bytes) at cost
  1000, so `Blob` writers — Base64, hex, checksums, … — are targets for any
  file, and a `Blob → X` bridge makes X universal too. A format with its own
  `Blob` reader (a decoder such as `base64`) replaces the implicit one.
- *Same-format conversion is first class*: `png → png`, `mp4 → mp4`,
  `zip → zip` re-encode through the format's own reader and writer with the
  `quality`/`preset`/… options. UIs offer it as "Compress" (the target entry
  carries `"compress": true`). It is never planned through the implicit Blob
  reader, so a format needs a real reader and writer to be compressible. A
  writer that only rewrites its own format (the PDF rewrite takes the PDF
  bytes as `Blob`) sets `Codec::same_format_only`, so other files reaching
  `Blob` through the implicit reader are not offered that target.
- *Pointless routes are excluded per bridge*: `Bridge::excluded_target_categories`
  lists target categories a bridge must not lead to (a still image made into
  a video never ends as a sound file; a waveform picture never becomes a
  video; video frames never become a document).
- *Target order* (`list_targets`, `cvt_targets_json`): the source's own
  category first, most popular first (a lossy target is not demoted there:
  PNG offers JPEG first); then real conversions to other categories, fewer
  bridges first, faithful before lossy, by popularity; the universal
  byte-level targets (Base64, checksums, …) last.

**Detection** (`engine/detect.hpp`) scores every format and takes the best:
a magic number scores 60 (1 byte) to 95 (8+ bytes); a `sniff` result is used
as returned (0–100); an extension match on a format with no content test
scores 78; content and extension agreeing adds 5; an extension whose format
*has* content tests that all failed scores 25; text-looking data falls back
to `txt` at 30. Consequences for format authors: give generic containers
their magic and specific formats a `sniff` (the ZIP-based office formats look
for their tell-tale member names); return ≥ 90 from `sniff` only when
certain; and never list a shared extension such as `txt` or `xml` without a
content test — `Registry::validate()` rejects two untested formats claiming
one extension.

**Conversion** (`engine/convert.hpp`): resolve and detect → plan → validate
options against each step's `OptionSpec`s → read → bridge → write → check
limits. Each step receives a context:

- `ctx.options` — values already coerced, clamped and defaulted from the
  step's specs; `ctx.options.get_int("level", 6)` just works.
- `ctx.progress` / `ctx.checkpoint(fraction, stage)` — report progress in
  long loops; `CVT_TRY(ctx.checkpoint(...))` also stops when the user cancels.
- `ctx.limits` — ceilings that defeat hostile input: `check_pixels`,
  `check_decompressed`, `check_archive_entries`, `check_output`, and
  `check_depth` (`max_depth`, default 256) for recursive parsers. Consult them
  **before** allocating anything sized by the input.
- `ctx.warn("...")` — non-fatal notes shown to the user.
- `ctx.registry` — for nested lookups (`ctx.registry->media()` for codecs).
- `ReadContext::input` is the whole source file (memory-mapped for file
  conversions). It stays valid until the *writer* returns, so streaming IRs
  may keep views into it; nothing else in a context outlives the call.

**Errors.** No exceptions cross any interface. Fallible functions return
`Result<T>` (`std::expected<T, Error>`); `fail(Status::Decode, "bad chunk {}", n)`
builds an error; `CVT_TRY(expr)` and `CVT_TRY_ASSIGN(auto x, expr)` propagate.
Use `Status::Decode` for damaged or unsupported input, `Encode` for content
the target cannot hold, `Limit` for exceeded ceilings, `InvalidArgument` for
bad option values. The engine converts stray exceptions and `bad_alloc` into
error results, but codecs must not rely on that.

**Registry.** `Registry::instance()` is built once (thread-safe) by
`register_all()` — every family in a fixed order, then the detection-only
database in `engine/known_formats.cpp` — and is immutable afterwards. A family
registration for an id that the database also knows wins field by field; the
database only fills gaps. There is no other global mutable state: conversions
are re-entrant and may run on any number of threads.

## Rules

These apply to every file in the core.

1. **In-house first.** Write formats, parsers and codecs yourself, against
   the specification. Do not add libraries, package managers, submodules,
   `FetchContent`, `ExternalProject` or `find_package`. The only exception is
   described under [Imported code](#imported-code).
2. **Offline and private.** No sockets, no telemetry, no child processes, no
   reading of files other than those handed in, no environment-dependent
   behaviour (locale, time zone, current time, random seeds). Output must be
   byte-identical for identical input and options on every platform.
3. **Hostile input.** Every reader must survive truncated, corrupt and
   malicious files: bounds-check every read (`util::ByteReader` does it for
   you), validate every size before allocating, cap recursion, honour
   `ctx.limits`, and return `Status::Decode`/`Status::Limit` instead of
   crashing. Run your tests under the `asan` preset.
4. **Language.** C++23 that also compiles as C++26. Available everywhere:
   `std::expected`, `std::span`, concepts, basic ranges, `std::format`,
   `std::byteswap`, `std::bit_cast`, deducing `this`, `constexpr` containers.
   Not available on all targets, do not use: `std::print`, `std::generator`,
   `std::flat_map`, modules, `std::views::zip`/`enumerate`,
   `std::ranges::fold_left`, `std::move_only_function`, `std::jthread`,
   floating-point `std::from_chars` (use `util::parse_double`), `<regex>` in
   hot paths. C++26-only features go behind feature-test macros. No compiler
   extensions, no SIMD intrinsics without a portable fallback, no unaligned
   pointer casts, no global mutable state, no locale-dependent functions.
   Remember 32-bit `size_t` (WASM, older Android): do size arithmetic in
   `std::uint64_t` and check before narrowing.
5. **Warnings are errors in spirit**: `-Wall -Wextra -Wpedantic` and `/W4`
   clean, explicit casts for narrowing conversions.
6. **No personal or machine data** anywhere: no absolute paths, user names,
   host names or e-mail addresses in code, comments, tests or fixtures; no
   author tags or tool attributions. File header:
   `// Copyright (c) Convertible contributors. MIT licence; see LICENSE.`
7. **Privacy by default in output**: `strip_metadata` defaults to true;
   writers must not invent identifying metadata (no "created by", no current
   date, no host name).
8. **The frozen interfaces** — `include/convertible/*`, `src/engine/*`,
   `src/ir/kind.hpp`, `src/ir/any.hpp`, `src/codec/codec.hpp`,
   `src/codec/registry.hpp`, `src/util/*`, `tests/support/*`, the CMake files —
   are shared by everyone. Do not edit them from a family branch; propose
   changes to the maintainers instead.

## Who owns what

A family engineer edits only:

- `src/families/<family>/**` — sources, private headers, optional `family.cmake`;
- the family's own IR header in `src/ir/`, **additively** (new fields with
  defaults, new helpers; never rename, remove or change meaning);
- `tests/families/<family>/**` and `tests/data/<family>/**`;
- `src/codec/ids.hpp` to append a codec id, `src/engine/known_formats.cpp` to
  append detection rows — append-only, one line each.

| Directory / entry point | Scope |
| --- | --- |
| `image` | common raster images: png, jpg, gif, bmp, tiff, ico, tga, pnm, … |
| `image_modern` | webp, avif, heic, jxl, jp2, exr, hdr, psd, camera raw, … |
| `vector` | svg, eps, emf/wmf, … and rasterisation |
| `audio_lossless` | wav, aiff, au, flac, alac, wavpack, … and their codecs |
| `audio_lossy` | mp3, aac/m4a, ogg vorbis, opus, ac3, amr, … and their codecs |
| `video` | containers and the frame/packet pipeline: mp4, mov, mkv, webm, avi, ts, flv, … |
| `codec_h264`, `codec_hevc`, `codec_vpx`, `codec_av1`, `codec_legacy` | video decoders and encoders, registered in `registry.media()` |
| `document` | markup and text documents: md, html, rtf, tex, rst, epub, … |
| `office` | docx, odt, xlsx, ods, pptx, odp and their binary predecessors |
| `pdf` | PDF reading and writing |
| `data` | json, yaml, toml, xml, ini, plist, msgpack, cbor, … |
| `table` | csv, tsv, fixed width, sqlite, playlists as tables, … |
| `archive` | zip, tar, gz, bz2, xz, zst, 7z, … |
| `font` | ttf, otf, woff, woff2, … |
| `subtitle` | srt, vtt, ass, ttml, … |
| `pim` | ics, vcf, … (`ir::Calendar`, `ir::Contacts`) |
| `mesh` | obj, stl, ply, gltf/glb, 3mf, … |
| `geo` | geojson, gpx, kml, … |
| `science` | fits, npy, mat, array containers, … |
| `code` | source code and developer formats |
| `encoding` | binary-to-text encodings, text transforms, checksums, and `txt` |

Each family implements `void register_<family>_family(Registry&)` in
`src/families/<family>/<family>_family.cpp` (already present as an empty
function, already called from `register_all.cpp`). Registration must be pure:
no I/O, no globals. A family may register codecs for any existing kind; small
families do not need a kind of their own (a playlist is a `Table`).

Shared ground:

- **A bridge belongs to the family that owns its source kind**
  (`Table → Document` to `table`, `Document → PlainText` to `document`,
  `Blob → Archive` to `archive`, `Video → Image`/`Audio` to `video`).
  `PlainText → Document` already exists (encoding family).
- **`txt` belongs to the encoding family** (read into / written from
  `PlainText` with encoding, BOM and line-ending options). Other families get
  plain text in and out through bridges instead of registering `txt` codecs.
- Two families may add codecs of different kinds to the same format id
  (html as `Document` and as `Table`): both call `add_format` with the fields
  they know; non-empty fields of the later call win, extensions are united.

### One owner per id (enforced by `Registry::validate()`)

- **Format description.** One family owns the `FormatInfo` of an id (name,
  category, MIME, detection). A family that only adds codecs of another kind
  to that id calls `add_known_format` (fills gaps, never overrides) or
  `add_format` with just the id; the detection database fills anything no
  family states. One id names one format: a format another family already
  uses under a different id is not registered again under a new spelling
  (the canonical spellings are listed at the top of `engine/known_formats.cpp`:
  the MPEG transport stream is `mpegts`, Sun raster `ras`, farbfeld `ff`, ...).
- **Readers and writers.** At most one reader and one writer per
  (format id, IR kind); a second one is a validation error, because which one
  ran would depend on costs and registration order. Containers and raw
  elementary streams (`mp4`, `mkv`, `h264`, `hevc`, `obu`, `m2v`, ...) belong
  to `video`; the codec families register decoders and encoders only.
- **Media codecs** (`codec::Registry`): one owner per (codec id, role). The
  owner calls `register_*`; a second owner registration is ignored and
  reported. A family that wants an implementation only when the owner is
  absent calls `register_*_fallback`, which never displaces anything and is
  replaced by a later owner registration, so the result does not depend on
  the order of `register_all()`. Owners: PCM/ADPCM/FLAC/ALAC/WavPack/GSM/IMA4
  → `audio_lossless`; `rawvideo` → `video` (its PCM codecs are fallbacks);
  `mjpeg`/`png` → `image`; `h264`, `hevc`, `vp8`/`vp9`, `av1` → their codec
  families; MPEG-1/2/4, H.263, Theora, ProRes, FFV1, HuffYUV, ... → `codec_legacy`.
- **Detection ties.** For the same confidence, a format a family describes
  beats a detection-only database entry, and an untested extension shared by
  a family format and a database entry resolves to the family's format (only
  two family formats, or two database entries, sharing one untested extension
  are a validation error). The `txt` fallback scores 30 and the text sniffers
  of the database up to 60: content-sniffed text formats must score above 60
  to win against them.

## Adding a format: a complete example

A reader and writer for binary PPM pictures in the `image` family. Three
files, no shared file touched.

`src/families/image/ppm.cpp`

```cpp
// Convertible core - image family: Netpbm PPM (binary "P6") pictures.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <charconv>
#include <format>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/image.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

// Header fields are separated by whitespace; '#' starts a comment line.
Result<std::uint32_t> header_number(util::ByteReader& reader) {
  while (!reader.at_end()) {
    const char c = static_cast<char>(reader.peek_u8());
    if (c == '#') {
      while (!reader.at_end() && reader.u8() != '\n') {}
    } else if (util::is_ascii_space(c)) {
      reader.skip(1);
    } else {
      break;
    }
  }
  std::uint32_t value = 0;
  std::size_t digits = 0;
  while (!reader.at_end() && util::is_ascii_digit(static_cast<char>(reader.peek_u8()))) {
    if (++digits > 9) return fail(Status::Decode, "PPM header number is too large");
    value = value * 10 + (reader.u8() - '0');
  }
  if (digits == 0) return fail(Status::Decode, "malformed PPM header");
  return value;
}

Result<AnyIR> read_ppm(ReadContext& ctx) {
  util::ByteReader reader(ctx.input);
  if (!reader.consume("P6")) return fail(Status::Decode, "not a binary PPM file");
  CVT_TRY_ASSIGN(const std::uint32_t width, header_number(reader));
  CVT_TRY_ASSIGN(const std::uint32_t height, header_number(reader));
  CVT_TRY_ASSIGN(const std::uint32_t max_value, header_number(reader));
  if (width == 0 || height == 0) return fail(Status::Decode, "PPM image has no pixels");
  if (max_value != 255) return fail(Status::Decode, "only 8-bit PPM files are supported (maxval {})", max_value);
  if (!util::is_ascii_space(static_cast<char>(reader.u8()))) return fail(Status::Decode, "malformed PPM header");

  // Never allocate from sizes found in a file without asking the limits first.
  CVT_TRY(ctx.limits.check_pixels(width, height));
  const std::uint64_t needed = std::uint64_t{width} * height * 3;
  if (reader.remaining() < needed) return fail(Status::Decode, "PPM pixel data is truncated");

  ir::Frame frame = ir::Frame::filled(width, height, {0, 0, 0, 255});
  for (std::uint32_t y = 0; y < height; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / height));  // progress + cancellation
    const std::span<const std::uint8_t> row = as_u8(reader.bytes(std::size_t{width} * 3));
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t at = frame.offset(x, y);
      frame.rgba[at] = row[x * 3];
      frame.rgba[at + 1] = row[x * 3 + 1];
      frame.rgba[at + 2] = row[x * 3 + 2];
    }
  }
  ir::Image image;
  image.frames.push_back(std::move(frame));
  return image;
}

Result<Bytes> write_ppm(const AnyIR& value, WriteContext& ctx) {
  const ir::Image& image = value.get<ir::Image>();  // the engine guarantees the kind
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  if (image.animated()) ctx.warn("PPM holds a single picture; only the first frame was written.");
  const ir::Frame& frame = image.frames.front();
  if (frame.has_transparency()) ctx.warn("PPM has no transparency; transparent areas were blended onto the background.");

  // A hex colour such as "ffffff", validated by hand because it is free text.
  const std::string background = ctx.options.get_string("background", "ffffff");
  std::uint32_t rgb = 0;
  const auto [end, error] = std::from_chars(background.data(), background.data() + background.size(), rgb, 16);
  if (background.size() != 6 || error != std::errc{} || end != background.data() + background.size()) {
    return fail(Status::InvalidArgument, "option 'background' must be six hexadecimal digits");
  }
  const std::array<std::uint32_t, 3> under = {(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF};

  util::ByteWriter out(std::size_t{frame.width} * frame.height * 3 + 32);
  out.text(std::format("P6\n{} {}\n255\n", frame.width, frame.height));
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    if ((y & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(y) / frame.height));
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      const std::size_t at = frame.offset(x, y);
      const std::uint32_t alpha = frame.rgba[at + 3];
      for (std::size_t c = 0; c < 3; ++c) {
        const std::uint32_t blended = (frame.rgba[at + c] * alpha + under[c] * (255 - alpha) + 127) / 255;
        out.u8(static_cast<std::uint8_t>(blended));
      }
    }
  }
  return out.take();
}

}  // namespace

void register_ppm(Registry& registry) {
  FormatInfo info;
  info.id = "ppm";
  info.name = "Portable pixmap";
  info.description = "Uncompressed Netpbm colour image.";
  info.category = Category::Image;
  info.extensions = {"ppm"};
  info.mime = "image/x-portable-pixmap";
  info.popularity = 20;
  info.signatures = {Signature::at(0, "'P6' 0A"), Signature::at(0, "'P6' 20"), Signature::at(0, "'P6' 0D")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "ppm";
  codec.kind = Kind::Image;
  codec.read = read_ppm;
  codec.write = write_ppm;
  codec.write_options = {
      OptionSpec::text("background", "Background colour", "ffffff",
                       "Colour placed behind transparent areas, as six hexadecimal digits (RRGGBB)."),
      options::strip_metadata(),  // standard option: declared for consistency (PPM cannot carry metadata anyway)
  };
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::image
```

`src/families/image/image.hpp` (the family's private header)

```cpp
// Convertible core - image family: internal declarations.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image {

void register_ppm(Registry& registry);

}  // namespace cvt::image
```

`src/families/image/image_family.cpp`

```cpp
// Convertible core - image family.
// Common raster images (png, jpg, gif, bmp, tiff, ico, tga, pnm, ...).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/image/image.hpp"

namespace cvt {

void register_image_family(Registry& registry) {
  image::register_ppm(registry);
}

}  // namespace cvt
```

`tests/families/image/ppm_test.cpp`

```cpp
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

using namespace std::string_view_literals;

namespace {
// Two pixels, red then blue, with a comment in the header. The `sv` suffix
// matters: the data contains NUL bytes, which would end a plain C string.
constexpr std::string_view tiny = "P6\n# made by hand\n2 1\n255\n\xFF\x00\x00\x00\x00\xFF"sv;
}  // namespace

TEST_CASE("image/ppm: decodes known pixels") {
  const auto image = test::read_as<ir::Image>("ppm", as_bytes(tiny));
  REQUIRE_OK(image);
  REQUIRE(image->frames.size() == 1);
  CHECK(image->width() == 2);
  CHECK(image->height() == 1);
  CHECK(image->frames[0].rgba == std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
}

TEST_CASE("image/ppm: encode and decode round trip is lossless") {
  ir::Image original;
  original.frames.push_back(test::gradient_frame(37, 21));
  const auto encoded = test::write_from("ppm", original);
  REQUIRE_OK(encoded);
  const auto decoded = test::read_as<ir::Image>("ppm", *encoded);
  REQUIRE_OK(decoded);
  CHECK(test::pixels_equal(original, *decoded));
}

TEST_CASE("image/ppm: transparency is blended onto the chosen background") {
  ir::Image image;
  image.frames.push_back(ir::Frame::filled(1, 1, {0, 0, 0, 0}));  // fully transparent
  std::vector<std::string> warnings;
  const auto encoded = test::write_from("ppm", image, R"({"background":"102030"})", &warnings);
  REQUIRE_OK(encoded);
  CHECK(test::str(*encoded) == "P6\n1 1\n255\n\x10\x20\x30");
  CHECK(warnings.size() == 1);
  CHECK_STATUS(test::write_from("ppm", image, R"({"background":"red"})"), Status::InvalidArgument);
}

TEST_CASE("image/ppm: damaged files are errors, never crashes") {
  CHECK_STATUS(test::read_as<ir::Image>("ppm", as_bytes(tiny.substr(0, tiny.size() - 1))), Status::Decode);  // truncated
  CHECK_STATUS(test::read_as<ir::Image>("ppm", as_bytes("P6\n99999 99999\n255\n")), Status::Limit);       // bomb
  CHECK_STATUS(test::read_as<ir::Image>("ppm", as_bytes("P6\n2 1\n65535\n")), Status::Decode);            // unsupported
  CHECK(test::fuzz_reader<ir::Image>("ppm", as_bytes(tiny)) > 0);  // every truncation + random corruption
}

TEST_CASE("image/ppm: is detected and converts through the engine") {
  CHECK(test::convert(tiny, "", "ppm", "", "picture.ppm").ok());  // detect, read, write: the "compress" route
  const test::Converted hex = test::convert(tiny, "ppm", "hex");
  CHECK(hex.ok());
}
```

What the example shows, and what every codec must do:

- **Describe the format** with `add_format`: lower-case id, extensions without
  dots (first = default for output), MIME type, popularity 0–100, signatures
  written as checked patterns (`"89 'PNG' 0D 0A 1A 0A"`, `??` wildcards,
  `FF&E0` masks — a malformed pattern does not compile).
- **Read defensively**: `ByteReader` never reads out of bounds; check
  `ctx.limits` before allocating; report progress and honour cancellation
  with `ctx.checkpoint`; return `Status::Decode` with a message a user can act
  on.
- **Write honestly**: when the target cannot represent something, either
  degrade and `ctx.warn(...)`, or fail with `Status::Encode`. Set
  `codec.lossy = true` when information is discarded by design (register the
  reader and the writer as two `Codec` entries if only one direction is lossy).
- **Declare options** as `OptionSpec`s; the engine validates and defaults
  them, publishes them to every UI, and warns about unknown keys.
- **Test** known content, round trips with a quality check, option handling,
  and hostile input (`test::fuzz_reader` feeds every truncation and a set of
  corruptions; run it under the `asan` preset).

The checklist before you call a format done is in `tests/README.md`.

## Bridges

```cpp
Bridge bridge;
bridge.from = Kind::Table;
bridge.to = Kind::Document;
bridge.cost = 15;
bridge.lossy = false;
bridge.label = "render table as document";
bridge.options = {OptionSpec::integer("sheet", "Sheet", 0, 0, 10000, "Index of the sheet to render.")};
bridge.fn = [](AnyIR value, Context& ctx) -> Result<AnyIR> {
  const ir::Table table = std::move(value).get<ir::Table>();
  ir::Document document;
  // ...
  return document;
};
registry.add_bridge(std::move(bridge));
```

A bridge takes its input by value (move from it) and must return the `to`
kind. Give lossy bridges a higher cost so that faithful routes win.

## Options

All option keys of one route (reader + bridges + writer) share one flat JSON
object, so name reader options so they cannot collide with writer options
(`source_encoding` vs `encoding`). The key `limits` is reserved.

Use the **standard options** from `engine/standard_options.hpp` for the
concepts they cover, so every format behaves alike:

| Key | Spec | Read it with |
| --- | --- | --- |
| `quality` (1–100, default 90) | `options::quality()` | `options::resolved_quality(ctx.options)` |
| `preset` (`high`, `balanced`, `small`) | `options::preset()` | folded into the two helpers around it |
| `compression_level` (0–9 or your scale) | `options::compression_level(max)` | `options::resolved_compression_level(ctx.options, max)` |
| `lossless` | `options::lossless()` | `options::resolved_lossless(ctx.options)` |
| `max_width`, `max_height`, `scale` | `options::max_width()` … | `options::resolved_image_size(ctx.options, {w, h})` |
| `strip_metadata` (default **true**) | `options::strip_metadata()` | `options::resolved_strip_metadata(ctx.options)` |
| `sample_rate`, `channels`, `bitrate_kbps` | `options::sample_rate()` … | `options::resolved_sample_rate(ctx.options, source)` … |
| `fps`, `width`, `height`, `video_bitrate_kbps`, `audio_bitrate_kbps` | `options::fps()` … | `options::resolved_video_size(...)`, `resolved_fps(...)` … |

`quality` means the same everywhere: 90 is visually/audibly transparent for
typical content, lower is smaller. An explicit `quality` wins over `preset`;
otherwise `high` = 90, `balanced` = 75, `small` = 50. A lossy writer should
declare at least `quality()` and `preset()`; a lossless one
`compression_level()` and `preset()`. Mark rarely needed options
`.advanced()`.

## Audio and video codecs

Containers and codecs are written by different people and meet only through
`src/codec/`:

- `codec/ids.hpp` — the canonical codec id strings (`codec::id::h264`,
  `codec::id::aac`, …). Use the constants; append new ids there.
- `codec/codec.hpp` — `VideoDecoder`, `VideoEncoder`, `AudioDecoder`,
  `AudioEncoder` with the send/receive/flush calling pattern documented at the
  top of the header, plus `CodecParams`, `VideoEncodeParams`,
  `AudioEncodeParams`.
- `codec/registry.hpp` — `codec::Registry`, reached as `registry.media()`
  during registration and `ctx.registry->media()` during conversion.
- `codec/colorspace.hpp` — `to_rgba`, `from_rgba`, `convert_frame` between
  RGBA8 and the planar YCbCr formats (BT.601/709/2020, limited/full range,
  correct chroma siting).

```cpp
// codec_h264 family
void register_codec_h264_family(Registry& registry) {
  registry.media().register_video_decoder(codec::id::h264, [] { return std::make_unique<H264Decoder>(); });
  registry.media().register_video_encoder(codec::id::h264, [] { return std::make_unique<H264Encoder>(); });
}

// a demuxer in the video family
auto decoder = ctx.registry->media().make_video_decoder(track.codec);
if (!decoder) return fail(Status::Decode, "no decoder available for {} video", track.codec);
```

Packaging is fixed by the convention documented at the top of
`codec/codec.hpp` (length-prefixed NAL units with avcC/hvcC, the `vpcC` box
body for VP8/VP9 — never copied to Matroska's CodecPrivate — av1C with
low-overhead OBUs, raw AAC with an AudioSpecificConfig). Decoders receive it,
encoders emit it, and every `ir::Track` carries it whatever the container
stored; containers convert to and from their own form. Decoders honour
`CodecParams::max_pixels` (the caller's pixel limit) and report non-fatal
problems through `VideoDecoder::take_warnings()`.

`ir::Video` is a stream, not a buffer: the reader returns functions
(`next_frame`, `next_audio`, and optionally compressed `tracks` for remuxing
without re-encoding) and the writer pulls. The functions may hold views into
`ReadContext::input` (kept alive until the writer returns) and shared decoder
state in a `std::shared_ptr`; they must not refer to the `ReadContext`
itself. Decoders deliver frames in their native pixel format; convert with
`colorspace.hpp` only where needed.

## Utilities

All in-house, all in `src/util/`, namespace `cvt::util` (types `Bytes`,
`ByteView` and `Json` live in `cvt`).

| Header | What it gives you |
| --- | --- |
| `bytes.hpp` | `Bytes`, `ByteView`, `as_bytes`/`as_chars`/`as_u8`, bounds-checked endian-aware `ByteReader`, `ByteWriter` |
| `text.hpp` | ASCII helpers, `split`/`trim`/`join`, `parse_int`/`parse_double`/`format_double` (locale-free), UTF-8 validation and repair, UTF-8/16/32 and Latin-1/Windows-1252 transcoding, BOM and encoding detection, line endings |
| `base64.hpp`, `hex.hpp` | encoders and strict decoders |
| `crc.hpp`, `hash.hpp` | CRC-32, Adler-32, MD5, SHA-1, SHA-256 |
| `deflate.hpp` | DEFLATE, zlib and gzip: one-shot functions with output limits and incremental `Inflater`/`Deflater` |
| `zip.hpp` | `ZipReader` (ZIP64, data descriptors, CP437/UTF-8 names, limits), deterministic `ZipWriter`, `sanitize_entry_path` |
| `json.hpp` | `cvt::Json` (ordered objects, int64/uint64/double), strict and lenient `parse_json`, `dump_json` |
| `xml.hpp` | `XmlDocument`/`XmlNode`/`XmlAttribute` DOM, `parse_xml` (namespaces, encodings, entity and depth limits, never any external access), `xml_to_string`, `xml_escape` |
| `fs.hpp`, `mapped_file.hpp` | UTF-8 paths on every platform, `read_file`/`write_file`, `TempDir`, read-only memory mapping |

If something general is missing, ask for it to be added here rather than
growing a private copy in a family.

## Tests

`tests/README.md` is the guide. In short: one executable
(`convertible_tests`), an in-house framework (`support/test.hpp`:
`TEST_CASE`, `SUBCASE`, `CHECK`, `REQUIRE`, `CHECK_OK`, `CHECK_STATUS`,
`CHECK_APPROX`, …), engine helpers (`support/test_support.hpp`: `convert`,
`read_as<T>`, `write_from`, `fuzz_reader`), fixtures (`support/fixtures.hpp`,
files under `tests/data/<family>/`) and quality metrics
(`support/metrics.hpp`: PSNR, SSIM, audio SNR with alignment, text
similarity, structural equality for data, tables and meshes).

**Every format gets tests that convert real files and check the result**:

1. *Decode* fixtures produced by independent, widely used software — never by
   our own encoder — and check them against known pixel, sample or text
   content.
2. *Encode → decode round trips* with a metric threshold per quality level,
   and a check that lower quality really produces smaller output.
3. *Truncated and corrupt input* returns an error and never crashes.

Fixtures are tiny (ideally under 50 KB), synthetic, free of personal or
machine data, and documented in a `SOURCES.md` next to them (tool, version,
exact command). See `tests/data/README.md`.

## Imported code

The default is to write formats in-house. Importing existing open-source
source code is allowed **only** for components where a from-scratch
implementation would be unrealistic or of lower quality — large codecs such
as H.264, HEVC, VP9, AV1, AAC, Opus, WebP or JPEG XL. Conditions:

- permissive licence only: MIT, BSD, zlib, Apache-2.0 or public domain; never
  GPL or LGPL;
- the source lives in-tree at `core/src/imported/<name>/`, with its licence
  file and original file headers intact; nothing is built or fetched from
  anywhere else;
- a `VENDOR.json` next to it:
  `{"name": "...", "version": "...", "license": "...", "url": "...", "files_modified": false}`
  (set `files_modified` to true and list the changes in a
  `CONVERTIBLE_CHANGES.md` beside it if you had to patch);
- it is compiled only through `cvt_add_imported()` from the owning family's
  `family.cmake`, and is used only behind that family's own wrapper — imported
  types never appear in `src/ir`, `src/engine`, `src/util` or public headers.

`src/families/<family>/family.cmake` is optional and is included automatically:

```cmake
# core/src/families/codec_av1/family.cmake
cvt_add_imported(some_decoder
  SOURCES src/a.c src/b.c
  INCLUDE_DIRS include
  PRIVATE_DEFINITIONS SOME_DECODER_NO_THREADS=1)
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_SOME_DECODER=1)
```

`cvt_add_imported` refuses to configure without `VENDOR.json` and a licence
file, compiles the code with warnings off, and folds the objects into
`convertible_core`, so apps still link exactly one library. A fragment must
not download, look up (`find_package`, pkg-config) or build anything outside
the tree.

## Notes for app developers

- Link `convertible::core` (`add_subdirectory(core)` from your CMake build —
  Android NDK, Emscripten, desktop — or build the static library and add it
  to an Xcode project) and include `convertible/convertible.h`.
- The API is synchronous and thread-safe; run conversions on a worker thread
  and use the progress callback to cancel.
- Render option forms from `cvt_options_schema_json`; offer "Compress" for
  the target whose `"compress"` flag is true; pass per-call safety limits in
  the reserved `"limits"` object of the options JSON.
- `cvt_convert_file` memory-maps its input and is the call for large files;
  the output is assembled in memory (default ceiling 2 GiB).
- Apple platforms: deployment target macOS 13.3 / iOS 16.4 or newer (the
  C++ runtime features the core uses start there). Emscripten: build and link
  with WebAssembly exception handling (`-fwasm-exceptions`; the core's CMake
  target propagates the flag).
- Exit codes of the CLI and `cvt_result_status` share one table:
  0 ok, 1 invalid argument, 2 unknown format, 3 unsupported conversion,
  4 decode, 5 encode, 6 I/O, 7 cancelled, 8 limit, 9 internal.
