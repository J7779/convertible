// Convertible core - code family: developer interchange formats (diff.cpp,
// devtools.cpp) - pure functions, unit-tested directly.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "util/json.hpp"

namespace cvt {
class Registry;
}  // namespace cvt

namespace cvt::code {

// ---------------------------------------------------------------------------- diffs

struct DiffLine {
  enum class Kind { Context, Added, Removed };
  Kind kind = Kind::Context;
  std::int64_t old_number = -1;  ///< line number in the old file, -1 for added lines
  std::int64_t new_number = -1;  ///< line number in the new file, -1 for removed lines
  std::string text;
};
struct DiffHunk {
  std::string header;  ///< "@@ -1,3 +1,4 @@ ..."
  std::vector<DiffLine> lines;
};
struct DiffFile {
  std::string header;  ///< "diff --git a/x b/x" when present
  std::string old_path;
  std::string new_path;
  std::vector<std::string> extra;  ///< "new file mode ...", "index ..."
  std::vector<DiffHunk> hunks;
  std::int64_t added = 0;
  std::int64_t removed = 0;
};
/// Unified diffs, git diffs and multi-file patches.
[[nodiscard]] Result<std::vector<DiffFile>> parse_diff(std::string_view text);

// ---------------------------------------------------------------------------- HTTP requests

/// A request as Data: {"method", "url", "headers": {name: value}, "body"?,
/// "form"?: [{"name","value"}], "auth"?: {"user","password"}, "options"?: {...}}.
[[nodiscard]] Result<Json> parse_curl(std::string_view command);
[[nodiscard]] Result<std::string> to_curl(const Json& request);
/// "GET /path HTTP/1.1" request text (Host header for the authority).
[[nodiscard]] Result<Json> parse_http_request(std::string_view text);
[[nodiscard]] Result<std::string> to_http_request(const Json& request);
[[nodiscard]] Result<std::string> to_fetch(const Json& request);
[[nodiscard]] Result<std::string> to_python_requests(const Json& request);

// ---------------------------------------------------------------------------- cron

/// "At 05:30 on every day-of-week from Monday through Friday."
[[nodiscard]] Result<std::string> describe_cron(std::string_view expression);
/// {"expression", "description", "minute": [..], "hour": [..], ...}.
[[nodiscard]] Result<Json> cron_to_data(std::string_view expression);

// ---------------------------------------------------------------------------- dates, identifiers, colours

/// Seconds since 1970-01-01T00:00:00Z of a date written as a Unix timestamp
/// (seconds, or milliseconds when above 10^11), ISO 8601 or RFC 2822.
struct Instant {
  std::int64_t seconds = 0;
  std::int32_t nanoseconds = 0;
  std::optional<std::int32_t> offset_minutes;  ///< the offset written in the input, if any
};
[[nodiscard]] Result<Instant> parse_datetime(std::string_view text);
/// `style`: iso8601, rfc2822, unix, unix_ms, or all (one line each).
[[nodiscard]] std::string format_datetime(const Instant& instant, std::string_view style);

/// A report on a UUID (version, variant, embedded time) or ULID.
[[nodiscard]] Result<std::string> describe_identifier(std::string_view text);

/// Parses #rgb, #rrggbb(aa), rgb()/rgba(), hsl()/hsla(), hwb(), oklch(), oklab()
/// and the CSS named colours; returns sRGB components 0..1 and alpha.
struct Rgba {
  double r = 0, g = 0, b = 0, a = 1;
};
[[nodiscard]] Result<Rgba> parse_color(std::string_view text);
/// `notation`: hex, rgb, hsl, hwb, oklch, oklab, name, all.
[[nodiscard]] std::string format_color(const Rgba& color, std::string_view notation);

void register_diff(Registry& registry);

}  // namespace cvt::code
