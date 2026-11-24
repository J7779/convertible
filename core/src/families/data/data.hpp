// Convertible core - data family: shared internal declarations.
//
// Every reader of this family produces an ir::Data tree in which values that
// JSON cannot express are tagged objects (see ir/data.hpp: `$binary`,
// `$date`, `$bigint`, `$decimal`, ...). After parsing, `finish_read` applies
// the user's policies in one place:
//   special_values  tagged (default) | plain | error
//                   plain: binary -> Base64 string, date -> ISO 8601 string,
//                   ObjectId -> hex string, UID -> integer, undefined -> null,
//                   CBOR tag -> its value, ...
//   big_numbers     exact (default: $bigint / $decimal) | double | string | error
// Non-finite `$float` tags always become doubles, so the tree has one
// spelling for NaN and the infinities.
//
// Every writer understands the tagged objects and writes them natively when
// the target has an equivalent (MessagePack bin, CBOR tag 1, TOML datetimes,
// YAML !!binary, plist <data>/<date>, ...) and as plain values otherwise.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "ir/any.hpp"
#include "ir/data.hpp"
#include "util/bytes.hpp"
#include "util/json.hpp"

namespace cvt {
class Registry;
struct FormatInfo;
}  // namespace cvt

namespace cvt::data {

// =========================================================================== tagged values

enum class TagKind : std::uint8_t {
  None,
  Binary,      ///< {"$binary"} or {"$binary","$subtype"}
  Ext,         ///< {"$ext","$binary"}
  Date,        ///< {"$date"}
  BigInt,      ///< {"$bigint"}
  Decimal,     ///< {"$decimal"}
  Float,       ///< {"$float"}
  Uid,         ///< {"$uid"}
  CborTag,     ///< {"$tag","$value"}
  Undefined,   ///< {"$undefined"}
  ObjectId,    ///< {"$oid"}
  Regex,       ///< {"$regex","$options"}
  Timestamp,   ///< {"$timestamp":{"t","i"}}
  MinKey,      ///< {"$minKey"}
  MaxKey,      ///< {"$maxKey"}
  Code,        ///< {"$code"}
  Symbol,      ///< {"$symbol"}
};

/// The kind of tagged object `value` is, or None when it is ordinary data.
/// Shapes are checked exactly (member names and payload types).
[[nodiscard]] TagKind tag_kind(const Json& value) noexcept;

[[nodiscard]] Json make_binary(ByteView bytes);
[[nodiscard]] Json make_binary_subtype(ByteView bytes, std::uint8_t subtype);
[[nodiscard]] Json make_ext(std::int64_t type, ByteView bytes);
[[nodiscard]] Json make_date(std::string iso8601);
[[nodiscard]] Json make_bigint(std::string digits);
[[nodiscard]] Json make_decimal(std::string text);
[[nodiscard]] Json make_uid(std::uint64_t uid);
[[nodiscard]] Json make_cbor_tag(std::uint64_t tag, Json value);
[[nodiscard]] Json make_undefined();
[[nodiscard]] Json make_object_id(ByteView twelve_bytes);
[[nodiscard]] Json make_regex(std::string pattern, std::string options);
[[nodiscard]] Json make_symbol(std::string name);
[[nodiscard]] Json make_code(std::string code);

/// Decoded payload of a Binary or Ext tag (nullopt when the Base64 is bad).
[[nodiscard]] std::optional<Bytes> tagged_bytes(const Json& value);
/// The string payload of a single-string tag ($date, $bigint, $decimal,
/// $float, $oid, $code, $symbol, $regex).
[[nodiscard]] std::string_view tag_text(const Json& value) noexcept;

/// Plain (JSON-only) rendering of a tagged value: Base64 string, ISO string,
/// raw number for $bigint/$decimal where the caller can write raw numbers
/// (here: the digits as a string), the integer of a UID, null for undefined,
/// and so on. Ordinary values are returned unchanged (shallow).
[[nodiscard]] Json plain_value(const Json& tagged);

/// Deep copy of `value` with every tagged object replaced by `plain_value`,
/// except $bigint / $decimal / $float when `keep_numbers` is set.
[[nodiscard]] Json untag(const Json& value, bool keep_numbers);

// =========================================================================== numbers

/// Turn the text of a decimal number literal (optional sign, digits, optional
/// fraction and exponent; already validated by the caller's grammar, without
/// digit separators) into a Json value: Int64 / UInt64 when integral and in
/// range, else $bigint; a double when the double prints back as the same
/// number, else $decimal (which also covers overflow and underflow).
[[nodiscard]] Json number_from_literal(std::string_view literal);

/// Integer literal in another base (2, 8, 16) with optional sign, digits only.
/// Values beyond 64 bits become $bigint. nullopt on bad digits.
[[nodiscard]] std::optional<Json> integer_from_digits(std::string_view digits, int base, bool negative);

/// Shortest round-trip text of a double in JSON style ("1.0", "1e+16",
/// "-0.0", "5e-324"). Non-finite values give "NaN", "Infinity", "-Infinity".
[[nodiscard]] std::string format_double(double value);
/// Exact text of an integer Json (Int64/UInt64).
[[nodiscard]] std::string format_integer(const Json& value);

/// Decimal digits (optional '-') <-> big-endian magnitude bytes. Used for
/// CBOR bignums and similar. `digits` must be valid; at most 4096 digits.
[[nodiscard]] std::optional<Bytes> decimal_to_magnitude(std::string_view digits, bool* negative);
[[nodiscard]] std::string magnitude_to_decimal(ByteView magnitude, bool negative);
/// Two's complement big-endian bytes -> decimal text.
[[nodiscard]] std::string twos_complement_to_decimal(ByteView bytes);

// =========================================================================== dates

/// A parsed ISO 8601 / RFC 3339 date-time with the parts the formats need.
struct DateTime {
  enum class Kind : std::uint8_t { Date, Time, Local, Offset };
  Kind kind = Kind::Offset;
  std::int64_t year = 1970;
  int month = 1, day = 1, hour = 0, minute = 0, second = 0;
  std::string fraction;     ///< fractional-second digits as written (no dot); may be empty
  int offset_minutes = 0;   ///< for Offset: minutes east of UTC
  bool zulu = true;         ///< for Offset: written as 'Z' (else as +hh:mm)

  /// Nanoseconds from the first 9 fraction digits.
  [[nodiscard]] std::uint32_t nanos() const noexcept;
};

/// Accepts "YYYY-MM-DD", "hh:mm[:ss[.f]]", "YYYY-MM-DD[T ]hh:mm[:ss[.f]][Z|±hh[:mm]]"
/// (T/Z case-insensitive), with range checks (including leap days and a
/// leap second of 60). nullopt otherwise.
[[nodiscard]] std::optional<DateTime> parse_datetime(std::string_view text) noexcept;
/// Canonical text: "YYYY-MM-DDThh:mm:ss[.f](Z|±hh:mm)" etc. Years beyond
/// 0000..9999 are written with a sign and at least 4 digits.
[[nodiscard]] std::string format_datetime(const DateTime& value);
/// Seconds since 1970-01-01T00:00:00Z (local times are taken as UTC).
[[nodiscard]] std::int64_t to_unix_seconds(const DateTime& value) noexcept;
/// UTC instant from Unix seconds + nanoseconds; `fraction_digits` 0 trims
/// trailing zeros of the fraction, otherwise pads to that many digits.
[[nodiscard]] DateTime from_unix(std::int64_t seconds, std::uint32_t nanos = 0);
[[nodiscard]] std::int64_t days_from_civil(std::int64_t y, int m, int d) noexcept;
void civil_from_days(std::int64_t days, std::int64_t& y, int& m, int& d) noexcept;

// =========================================================================== text input

/// The input as UTF-8: a BOM selects UTF-8/16/32; otherwise the bytes must be
/// UTF-8 (`legacy_fallback`: invalid UTF-8 is read as Windows-1252 instead of
/// failing). NUL bytes in BOM-less input are an error for text formats.
[[nodiscard]] Result<std::string> input_text(const ReadContext& ctx, std::string_view format_name,
                                             bool legacy_fallback = false);

/// 1-based line and column (in code points) of `offset` within `text`.
struct TextPosition {
  std::size_t line = 1;
  std::size_t column = 1;
};
[[nodiscard]] TextPosition position_of(std::string_view text, std::size_t offset) noexcept;

/// Status::Decode "invalid <format> at line L, column C: <message>".
[[nodiscard]] std::unexpected<Error> syntax_error(std::string_view format_name, std::string_view text,
                                                  std::size_t offset, std::string_view message);

/// Append the JSON-style escaped, double-quoted form of `text` (valid UTF-8
/// expected; malformed bytes become U+FFFD). `ascii` escapes everything
/// above U+007E as \uXXXX (surrogate pairs beyond the BMP).
void append_json_string(std::string& out, std::string_view text, bool ascii = false);

/// Make text valid UTF-8 (malformed sequences -> U+FFFD), counting repairs.
[[nodiscard]] std::string repair_utf8(std::string_view text, std::size_t* repaired = nullptr);

// =========================================================================== policies

/// Settings shared by the readers, taken from the options (see the option
/// helpers below). The recursion guard of every recursive parser.
struct ReadPolicy {
  std::size_t max_depth = 256;
  enum class Duplicates : std::uint8_t { Last, First, Error } duplicates = Duplicates::Last;
};
[[nodiscard]] ReadPolicy read_policy(const Options& options);

/// Insert a member honouring the duplicate-key policy. `where` is used in the
/// error message.
[[nodiscard]] Result<void> put_member(Json& object, std::string key, Json value, const ReadPolicy& policy);

/// Apply `special_values` and `big_numbers` to a freshly read tree and turn
/// $float tags into doubles. Also enforces the `max_depth` option on trees
/// built without recursion. Call once at the end of every reader.
[[nodiscard]] Result<void> finish_read(Json& value, const Context& ctx);

/// Depth of the deepest container (a scalar has depth 0). Iterative.
[[nodiscard]] std::size_t json_depth(const Json& value);

/// Writers recurse; refuse trees deeper than this.
inline constexpr std::size_t max_write_depth = 1000;
[[nodiscard]] Result<void> check_write_depth(const Json& value, std::string_view format_name);

/// Members of an object, sorted by key (byte order) or in insertion order.
[[nodiscard]] std::vector<const Json::Member*> members_of(const Json& object, bool sort);

/// Same-format conversion ("compress"): true when the writer runs on data
/// read from the same format.
[[nodiscard]] inline bool same_format(const WriteContext& ctx) noexcept {
  return !ctx.source_format_id.empty() && ctx.source_format_id == ctx.format_id;
}

// --------------------------------------------------------------------------- option specs

/// special_values, big_numbers (advanced), max_depth (advanced).
[[nodiscard]] std::vector<OptionSpec> common_read_options(bool with_duplicates = true, bool with_numbers = true);
[[nodiscard]] OptionSpec indent_option(int default_indent = 2);
[[nodiscard]] OptionSpec sort_keys_option();
[[nodiscard]] OptionSpec documents_option();
[[nodiscard]] OptionSpec document_index_option();

/// Multi-document inputs (YAML streams, NDJSON, ...): pick per the
/// `documents` option ("all" -> array, "first", "index" + document_index).
[[nodiscard]] Result<Json> select_documents(std::vector<Json> documents, const Options& options,
                                            std::string_view format_name);

// --------------------------------------------------------------------------- registration

/// Shorthand used by the registration functions below.
struct CodecDef {
  std::string format_id = {};
  std::function<Result<AnyIR>(ReadContext& ctx)> read = {};
  std::function<Result<Bytes>(const AnyIR& value, WriteContext& ctx)> write = {};
  std::vector<OptionSpec> read_options = {};
  std::vector<OptionSpec> write_options = {};
  bool lossy = false;
  int read_cost = 10;
  int write_cost = 10;
};
void add_codec(Registry& registry, CodecDef def);

/// Wrap a text parser `Result<Json>(std::string_view text, ReadContext&)` into
/// a codec read function (input decoding + finish_read).
using TextParser = Result<Json> (*)(std::string_view text, ReadContext& ctx);
[[nodiscard]] std::function<Result<AnyIR>(ReadContext&)> text_reader(TextParser parser, std::string format_name,
                                                                     bool legacy_fallback = false);
using BinaryParser = Result<Json> (*)(ByteView input, ReadContext& ctx);
[[nodiscard]] std::function<Result<AnyIR>(ReadContext&)> binary_reader(BinaryParser parser);
using TextWriter = Result<std::string> (*)(const Json& value, WriteContext& ctx);
[[nodiscard]] std::function<Result<Bytes>(const AnyIR&, WriteContext&)> text_writer(TextWriter writer,
                                                                                    std::string format_name);
using BinaryWriter = Result<Bytes> (*)(const Json& value, WriteContext& ctx);
[[nodiscard]] std::function<Result<Bytes>(const AnyIR&, WriteContext&)> binary_writer(BinaryWriter writer,
                                                                                     std::string format_name);

/// Sniff helper: the lower-case extension of `filename` is one of `exts`.
[[nodiscard]] bool has_extension(std::string_view filename, std::initializer_list<std::string_view> exts) noexcept;
/// The first `limit` bytes as text with a UTF-8 BOM and leading white space removed.
[[nodiscard]] std::string_view text_head(ByteView data, std::size_t limit = 4096) noexcept;

// --------------------------------------------------------------------------- JSON text

/// How the JSON writers spell things. Tagged $bigint/$decimal are written as
/// raw number literals (exact); other tagged objects as the objects they are.
struct JsonStyle {
  int indent = 2;  ///< < 0: compact single line
  bool sort_keys = false;
  bool ascii_only = false;
  bool escape_slash = false;
  enum class NonFinite : std::uint8_t { Tagged, Null, Literal, String, Error } non_finite = NonFinite::Tagged;
  bool json5 = false;  ///< unquoted identifier keys, NaN/Infinity literals
  std::string_view newline = "\n";
};
[[nodiscard]] Result<std::string> write_json_text(const Json& value, const JsonStyle& style);

// --------------------------------------------------------------------------- the formats

void register_json_formats(Registry& registry);
void register_hjson(Registry& registry);
void register_yaml(Registry& registry);
void register_toml(Registry& registry);
void register_xml_data(Registry& registry);
void register_plist(Registry& registry);
void register_config_formats(Registry& registry);   // ini, properties, env, reg
void register_web_formats(Registry& registry);      // query strings, HTTP headers
void register_lisp_formats(Registry& registry);     // edn, s-expressions
void register_kdl(Registry& registry);
void register_hocon(Registry& registry);
void register_ron(Registry& registry);
void register_cson(Registry& registry);
void register_script_literals(Registry& registry);  // lua tables, php serialize, python literals
void register_msgpack(Registry& registry);
void register_cbor(Registry& registry);
void register_bson(Registry& registry);
void register_ubjson(Registry& registry);           // ubjson, bjdata
void register_smile(Registry& registry);
void register_ion(Registry& registry);
void register_bencode(Registry& registry);
void register_protobuf(Registry& registry);
void register_avro(Registry& registry);
void register_flexbuffers(Registry& registry);
void register_pickle(Registry& registry);
void register_data_bridges(Registry& registry);

}  // namespace cvt::data
