// Convertible core - the format registry: formats, codecs and bridges.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codec/registry.hpp"
#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/any.hpp"
#include "ir/kind.hpp"
#include "util/bytes.hpp"

namespace cvt {

// --------------------------------------------------------------------------- categories

/// Coarse grouping of formats, used by UIs for filtering and by the planner
/// to rank targets ("same category first").
enum class Category {
  Image,
  Audio,
  Video,
  Document,
  Ebook,
  Spreadsheet,
  Presentation,
  Data,
  Archive,
  Font,
  Subtitle,
  Model3D,
  Geo,
  Pim,
  Code,
  Encoding,
  Vector,
  Other,
};

/// Lower-case name used in JSON: "image", "audio", "video", "document",
/// "ebook", "spreadsheet", "presentation", "data", "archive", "font",
/// "subtitle", "model3d", "geo", "pim", "code", "encoding", "vector", "other".
[[nodiscard]] std::string_view category_name(Category category) noexcept;
[[nodiscard]] std::optional<Category> parse_category(std::string_view name) noexcept;

// --------------------------------------------------------------------------- signatures

namespace detail {
/// Validates the pattern grammar accepted by `SignaturePattern` (see there).
/// Returns the number of bytes the pattern describes, or -1 when malformed.
[[nodiscard]] constexpr int signature_pattern_length(std::string_view pattern) noexcept {
  const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
  const auto nibble = [&](char c) { return hex(c) || c == '?'; };
  int count = 0;
  std::size_t i = 0;
  while (i < pattern.size()) {
    const char c = pattern[i];
    if (c == ' ') {
      ++i;
    } else if (c == '\'') {
      const std::size_t close = pattern.find('\'', i + 1);
      if (close == std::string_view::npos || close == i + 1) return -1;
      count += static_cast<int>(close - i - 1);
      i = close + 1;
    } else {
      if (i + 1 >= pattern.size() || !nibble(c) || !nibble(pattern[i + 1])) return -1;
      i += 2;
      if (i < pattern.size() && pattern[i] == '&') {
        if (i + 2 >= pattern.size() || !hex(pattern[i + 1]) || !hex(pattern[i + 2])) return -1;
        if (c == '?' || pattern[i - 1] == '?') return -1;
        i += 3;
      }
      if (i < pattern.size() && pattern[i] != ' ') return -1;
      ++count;
    }
  }
  return count > 0 ? count : -1;
}
}  // namespace detail

/// A magic-number pattern, checked at compile time when written as a literal.
///
/// Grammar (tokens separated by spaces):
///   `89`        one byte, two hex digits
///   `??`        any byte
///   `4?` `?F`   high / low nibble only
///   `FF&E0`     byte compared under an explicit mask: (input & 0xE0) == (0xFF & 0xE0)
///   `'RIFF'`    literal ASCII characters (anything except a single quote)
///
/// Examples: "89 'PNG' 0D 0A 1A 0A", "'RIFF' ?? ?? ?? ?? 'WEBP'", "FF D8 FF".
struct SignaturePattern {
  std::string_view text;

  /// The empty pattern: matches nothing, used for unused table slots.
  constexpr SignaturePattern() noexcept = default;

  template <class T>
    requires std::convertible_to<const T&, std::string_view>
  consteval SignaturePattern(const T& pattern) : text(pattern) {  // NOLINT(google-explicit-constructor)
    if (detail::signature_pattern_length(text) < 0) throw "malformed signature pattern";
  }
};

/// Bytes expected at a fixed offset from the start of a file.
struct Signature {
  /// Where the pattern starts, in bytes from the beginning of the file.
  std::size_t offset = 0;
  /// The expected bytes.
  Bytes bytes;
  /// Per-byte mask, same length as `bytes`, or empty for an exact match.
  /// A file matches when `(file[offset + i] & mask[i]) == (bytes[i] & mask[i])`.
  Bytes mask;

  /// Build from a pattern literal (validated at compile time):
  ///   Signature::at(0, "89 'PNG' 0D 0A 1A 0A")
  [[nodiscard]] static Signature at(std::size_t offset, SignaturePattern pattern);
  /// Build from a pattern only known at run time; nullopt when malformed.
  [[nodiscard]] static std::optional<Signature> parse(std::size_t offset, std::string_view pattern);

  [[nodiscard]] bool matches(ByteView data) const noexcept;
  /// Number of bits the pattern actually constrains; longer and stricter
  /// patterns give more confident detections.
  [[nodiscard]] std::size_t significant_bits() const noexcept;
};

// --------------------------------------------------------------------------- formats

/// Everything the engine knows about a file format, independent of whether
/// it can be read or written.
struct FormatInfo {
  /// Lower-case canonical id, unique across the registry: letters, digits,
  /// '.', '-', '_', '+' (e.g. "png", "tar.gz", "utf-16le").
  std::string id;
  /// Human name, e.g. "PNG image".
  std::string name;
  /// One sentence for format pickers; may be empty.
  std::string description;
  Category category = Category::Other;
  /// File extensions, lower-case, without the dot; the first one is the
  /// default for output files. May contain dots ("tar.gz").
  std::vector<std::string> extensions;
  /// IANA media type; empty means "application/octet-stream".
  std::string mime;
  /// 0..100: how commonly the format is used. Sort key for UIs and
  /// tie-breaker for detection and target ranking.
  int popularity = 0;
  /// Magic numbers. Any one matching identifies the format.
  std::vector<Signature> signatures;
  /// Optional content inspection for formats that magic numbers cannot pin
  /// down (ZIP-based documents, text formats...). Receives the whole file
  /// and the file name (may be empty); returns a confidence 0..100, where 0
  /// means "not this format". Must be fast, re-entrant and must not throw.
  std::function<int(std::span<const std::byte> data, std::string_view filename)> sniff;
};

// --------------------------------------------------------------------------- codecs

/// Reads or writes one format as one IR kind.
///
/// A format may have several codecs of different kinds (html: Document
/// reader + writer, and a Table writer) and even several for the same kind;
/// the planner uses the cheapest suitable one.
struct Codec {
  /// Id of the format (see FormatInfo::id).
  std::string format_id;
  /// The IR kind `read` produces and `write` consumes.
  Kind kind = Kind::Blob;
  /// Decode `ctx.input`. Must return a value of `kind`. Null = this codec
  /// cannot read. The returned IR may keep views into `ctx.input` (the engine
  /// keeps the input alive until the writer has returned) but must not refer
  /// to `ctx` itself, which dies when `read` returns. Report malformed input as Status::Decode; never crash,
  /// never read out of bounds, honour `ctx.limits` and `ctx.progress`.
  std::function<Result<AnyIR>(ReadContext& ctx)> read;
  /// Encode a value of `kind`. Null = this codec cannot write. Report
  /// content the format cannot represent as Status::Encode (or degrade
  /// gracefully and `ctx.warn`).
  std::function<Result<Bytes>(const AnyIR& value, WriteContext& ctx)> write;
  /// Relative cost of each direction; lower is preferred. Guideline:
  /// 10 = fast and faithful, 20-50 = slow or approximate. The engine's
  /// implicit "read anything as Blob" step costs `implicit_blob_read_cost`.
  int read_cost = 10;
  int write_cost = 10;
  /// True when this codec discards information (lossy compression, dropped
  /// structure). Applies to every direction the entry provides; register the
  /// reader and the writer as two entries when they differ.
  bool lossy = false;
  /// Options understood by `write` / `read`.
  std::vector<OptionSpec> write_options;
  std::vector<OptionSpec> read_options;
};

/// Cost of the engine-provided "read any format as ir::Blob" step.
inline constexpr int implicit_blob_read_cost = 1000;

/// Converts a value of one IR kind into another kind (Table -> Document,
/// Document -> PlainText, Blob -> Archive, ...). The planner may chain up to
/// two bridges between a reader and a writer.
struct Bridge {
  Kind from = Kind::Blob;
  Kind to = Kind::Blob;
  /// Takes the value by value (move from it freely); must return kind `to`.
  std::function<Result<AnyIR>(AnyIR value, Context& ctx)> fn;
  /// Relative cost; guideline 5-30. Prefer higher costs for bridges that
  /// lose a lot, so that direct routes win.
  int cost = 10;
  /// True when information is discarded on the way.
  bool lossy = false;
  /// Short description for diagnostics, e.g. "render table as document".
  std::string label;
  /// Options understood by `fn`.
  std::vector<OptionSpec> options;
};

// --------------------------------------------------------------------------- registry

/// The catalogue of everything the engine can do.
///
/// The process-wide instance is built once, on first use, by
/// `register_all()` and is immutable afterwards, so it can be used from any
/// thread without locking. Tests build their own instances.
class Registry {
 public:
  Registry() = default;
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  Registry(Registry&&) = default;
  Registry& operator=(Registry&&) = default;

  /// The shared, fully populated registry.
  [[nodiscard]] static const Registry& instance();

  // -- registration (single-threaded, before the registry is shared) --------

  /// Describe a format. Calling it again for the same id MERGES: non-empty
  /// scalar fields of the later call replace earlier ones, extensions are
  /// united (later call's first), signatures are appended, a later `sniff`
  /// replaces an earlier one. Ids are normalised to lower case.
  void add_format(FormatInfo info);
  /// Describe a format with low priority: only fields that no `add_format`
  /// call provides are taken from it, in whatever order the calls happen.
  /// Used by the detection-only known-formats database.
  void add_known_format(FormatInfo info);
  void add_codec(Codec codec);
  void add_bridge(Bridge bridge);
  /// The audio/video decoder and encoder factories (see codec/registry.hpp).
  /// Families register implementations here during registration; container
  /// codecs look them up at conversion time through `ctx.registry->media()`.
  [[nodiscard]] codec::Registry& media() noexcept { return media_; }
  [[nodiscard]] const codec::Registry& media() const noexcept { return media_; }

  /// Make sure every format referenced by a codec has a
  /// FormatInfo (creating minimal ones where missing). Called once after all
  /// registrations; `instance()` does it for the shared registry.
  void finalize();

  // -- lookup ----------------------------------------------------------------

  /// The format with exactly this canonical id, or nullptr.
  [[nodiscard]] const FormatInfo* find(std::string_view id) const;
  /// Forgiving lookup for user input: trims, ignores case and a leading dot,
  /// then tries the id and finally the extensions (most popular format first).
  [[nodiscard]] const FormatInfo* resolve(std::string_view id_or_extension) const;
  /// All formats, most popular first, ties by id.
  [[nodiscard]] std::vector<const FormatInfo*> formats() const;
  /// Formats listing this extension (lower-case, no dot), most popular first.
  [[nodiscard]] std::vector<const FormatInfo*> formats_with_extension(std::string_view extension) const;

  /// Every codec, in registration order.
  [[nodiscard]] std::span<const Codec> codecs() const noexcept { return codecs_; }
  /// Codecs of one format, in registration order.
  [[nodiscard]] std::vector<const Codec*> codecs_for(std::string_view format_id) const;
  [[nodiscard]] std::span<const Bridge> bridges() const noexcept { return bridges_; }

  /// True when some codec can read / write the format (the implicit Blob
  /// reader does not count).
  [[nodiscard]] bool has_reader(std::string_view format_id) const;
  [[nodiscard]] bool has_writer(std::string_view format_id) const;

  /// Consistency problems (bad ids, codecs without functions, signatures
  /// with mismatched masks, bridges to their own kind, ...). Empty when the
  /// registry is sound; the test suite asserts this for the shared instance.
  [[nodiscard]] std::vector<std::string> validate() const;

 private:
  struct Entry {
    FormatInfo info;
    bool authoritative = false;  // described by add_format (not only by the known-formats database)
  };
  Entry& entry_for(std::string id);

  std::map<std::string, Entry, std::less<>> formats_;
  std::vector<Codec> codecs_;
  std::map<std::string, std::vector<std::size_t>, std::less<>> codecs_by_format_;
  std::vector<Bridge> bridges_;
  codec::Registry media_;
};

/// Normalise a format id or extension typed by a user: trim, lower-case,
/// drop one leading dot.
[[nodiscard]] std::string normalize_format_id(std::string_view text);

}  // namespace cvt
