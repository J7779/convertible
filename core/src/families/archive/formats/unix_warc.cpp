// Convertible core - archive family: WARC web archives (ISO 28500, WARC 1.0 and 1.1), read only.
//
// A WARC file is a sequence of records: a version line ("WARC/1.1"), named
// header fields, an empty line, a block of Content-Length bytes and two line
// breaks. ".warc.gz" files compress every record as its own gzip member; the
// members are simply joined.
//
// Only "response" and "resource" records become entries (warcinfo, request,
// metadata, revisit, conversion and continuation records describe the crawl,
// not files). Names come from WARC-Target-URI: "host/path" with the query and
// fragment dropped, percent escapes decoded, dot segments resolved, characters
// that file systems reject replaced by '_', "index.html" added to
// directory-like paths, and "~2", "~3"... inserted before the extension of a
// name seen before. HTTP responses lose their status line and headers; a
// chunked transfer coding and gzip / deflate / br / zstd content codings are
// undone. The entry time is WARC-Date. WARC-Block-Digest (sha1, base32) is
// checked; a mismatch is reported as a warning.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <format>
#include <map>

#include "families/archive/compression.hpp"
#include "families/archive/formats/unix_internal.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::archive::unixfmt {

namespace {

struct Fields {
  std::vector<std::pair<std::string, std::string>> items;
  [[nodiscard]] std::string_view get(std::string_view name) const {
    for (const auto& [k, v] : items) {
      if (util::iequals(k, name)) return v;
    }
    return {};
  }
};

// Header lines from `text` up to the first empty line; returns the bytes consumed.
std::optional<std::size_t> parse_fields(std::string_view text, Fields& out) {
  std::size_t pos = 0;
  while (true) {
    const std::size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) return std::nullopt;
    std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.empty()) return pos;
    if ((line.front() == ' ' || line.front() == '\t') && !out.items.empty()) {  // folded continuation
      out.items.back().second += ' ';
      out.items.back().second += util::trim(line);
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    out.items.emplace_back(std::string(util::trim(line.substr(0, colon))), std::string(util::trim(line.substr(colon + 1))));
    if (out.items.size() > 10000) return std::nullopt;
  }
}

std::optional<std::array<std::uint8_t, 20>> base32_sha1(std::string_view text) {
  if (text.size() != 32) return std::nullopt;
  std::array<std::uint8_t, 20> out{};
  std::uint64_t acc = 0;
  int bits = 0;
  std::size_t n = 0;
  for (const char c : text) {
    int v = -1;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    if (c >= 'a' && c <= 'z') v = c - 'a';
    if (c >= '2' && c <= '7') v = c - '2' + 26;
    if (v < 0) return std::nullopt;
    acc = (acc << 5) | static_cast<std::uint64_t>(v);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (n < out.size()) out[n++] = static_cast<std::uint8_t>((acc >> bits) & 0xFFu);
    }
  }
  return n == out.size() ? std::optional(out) : std::nullopt;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percent_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
      out += static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return util::sanitize_utf8(out);
}

std::string clean_segment(std::string_view s) {
  std::string out;
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || std::string_view("<>:\"\\|?*/").find(c) != std::string_view::npos) {
      out += '_';
    } else {
      out += c;
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.back() = '_';
  return out;
}

// "http://Example.com:8080/a/../b/?q#f" -> "example.com_8080/b/index.html".
std::string name_from_uri(std::string_view uri) {
  uri = util::trim(uri);
  if (uri.size() >= 2 && uri.front() == '<' && uri.back() == '>') uri = uri.substr(1, uri.size() - 2);
  uri = uri.substr(0, uri.find('#'));
  std::string host;
  std::string_view path;
  const std::size_t scheme_end = uri.find("://");
  if (scheme_end != std::string_view::npos) {
    const std::string_view rest = uri.substr(scheme_end + 3);
    const std::size_t slash = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, slash);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) authority = authority.substr(at + 1);
    host = util::to_lower(authority);
    std::replace(host.begin(), host.end(), ':', '_');
    path = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
  } else {
    // "urn:x:y", "dns:example.com": the scheme becomes a directory.
    const std::size_t colon = uri.find(':');
    host = colon == std::string_view::npos ? std::string("other") : util::to_lower(uri.substr(0, colon));
    path = colon == std::string_view::npos ? uri : uri.substr(colon + 1);
  }
  path = path.substr(0, path.find('?'));
  std::vector<std::string> segments;
  const std::vector<std::string_view> parts = util::split(path, '/', true);
  for (const std::string_view part : parts) {
    const std::string decoded = percent_decode(part);
    if (decoded.empty() || decoded == ".") continue;
    if (decoded == "..") {
      if (!segments.empty()) segments.pop_back();
      continue;
    }
    segments.push_back(clean_segment(decoded));
  }
  const bool directory_like = path.empty() || path.ends_with('/') || path.ends_with("/.") || path.ends_with("/..");
  std::string name = clean_segment(host.empty() ? std::string("unknown") : host);
  for (const std::string& s : segments) name += "/" + s;
  if (directory_like || segments.empty()) name += "/index.html";
  return name;
}

std::string with_suffix(const std::string& name, std::size_t n) {
  const std::size_t slash = name.rfind('/');
  const std::size_t dot = name.rfind('.');
  const std::string tag = std::format("~{}", n);
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == slash + 1) return name + tag;
  return name.substr(0, dot) + tag + name.substr(dot);
}

Result<Bytes> dechunk(ByteView body, std::uint64_t budget) {
  Bytes out;
  std::size_t pos = 0;
  const std::string_view text = as_chars(body);
  while (true) {
    const std::size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) return fail(Status::Decode, "chunked HTTP body is truncated");
    std::string_view line = text.substr(pos, nl - pos);
    line = line.substr(0, line.find(';'));
    const auto size = parse_field(util::trim(line), 16);
    if (!size) return fail(Status::Decode, "chunked HTTP body is damaged");
    pos = nl + 1;
    if (*size == 0) break;
    if (*size > text.size() - pos) return fail(Status::Decode, "chunked HTTP body is truncated");
    if (out.size() + *size > budget) return fail(Status::Limit, "the HTTP body exceeds the size limit");
    append(out, body.subspan(pos, static_cast<std::size_t>(*size)));
    pos += static_cast<std::size_t>(*size);
    if (pos < text.size() && text[pos] == '\r') ++pos;
    if (pos < text.size() && text[pos] == '\n') ++pos;
  }
  return out;
}

struct Payload {
  Bytes bytes;
  bool decoded_ok = true;
};

// The body of an HTTP response block, with transfer and content codings undone.
Result<Payload> http_body(ByteView block, EntrySink& sink) {
  Payload out;
  const std::string_view text = as_chars(block);
  Fields headers;
  const std::size_t first_nl = text.find('\n');
  if (first_nl == std::string_view::npos) {
    out.bytes = to_bytes(block);
    return out;
  }
  const auto consumed = parse_fields(text.substr(first_nl + 1), headers);
  if (!consumed) {
    out.bytes.clear();  // headers only (truncated capture)
    return out;
  }
  const ByteView body = block.subspan(first_nl + 1 + *consumed);
  Bytes raw;
  if (util::to_lower(headers.get("Transfer-Encoding")).find("chunked") != std::string::npos) {
    auto dechunked = dechunk(body, sink.budget());
    if (!dechunked) {
      if (dechunked.error().code == Status::Limit) return std::unexpected(std::move(dechunked).error());
      raw = to_bytes(body);
      out.decoded_ok = false;
    } else {
      raw = std::move(*dechunked);
    }
  } else {
    raw = to_bytes(body);
  }
  const std::string coding = util::to_lower(util::trim(headers.get("Content-Encoding")));
  std::optional<compression::Method> method;
  if (coding == "gzip" || coding == "x-gzip") method = compression::Method::Gzip;
  if (coding == "deflate") method = compression::detect(raw) == compression::Method::Zlib ? compression::Method::Zlib : compression::Method::Deflate;
  if (coding == "br") method = compression::Method::Brotli;
  if (coding == "zstd") method = compression::Method::Zstd;
  if (method && !raw.empty() && compression::can_decode(*method)) {
    compression::DecodeOptions d;
    d.max_output = sink.budget();
    auto decoded = compression::decompress(*method, raw, d);
    if (decoded) {
      out.bytes = std::move(*decoded);
      return out;
    }
    if (decoded.error().code == Status::Limit) return std::unexpected(std::move(decoded).error());
    out.decoded_ok = false;
  }
  out.bytes = std::move(raw);
  return out;
}

}  // namespace

int sniff_warc(ByteView data) noexcept {
  if (data.size() >= 7 && as_chars(data.first(7)) == "WARC/1.") return 95;
  if (data.size() < 18 || compression::detect(data) != compression::Method::Gzip ||
      !compression::can_decode(compression::Method::Gzip)) {
    return 0;
  }
  compression::DecodeOptions d;
  d.stop_after = 7;
  d.max_output = 1u << 20;
  d.verify_checksums = false;
  const auto head = compression::decompress(compression::Method::Gzip, data.first(std::min<std::size_t>(data.size(), 4096)), d);
  return head && head->size() >= 7 && as_chars(ByteView(*head).first(7)) == "WARC/1." ? 94 : 0;
}

Result<ir::Archive> read_warc(ReadContext& ctx) {
  EntrySink sink(ctx, ctx.input.size());
  Bytes inflated;
  ByteView data = ctx.input;
  if (compression::detect(data) == compression::Method::Gzip) {
    compression::DecodeOptions d;
    d.max_output = sink.budget();
    d.progress = ctx.progress.sub(0.0, 0.5);
    CVT_TRY_ASSIGN(inflated, compression::decompress(compression::Method::Gzip, data, d));
    data = inflated;
  }
  const std::string_view text = as_chars(data);
  std::map<std::string, std::size_t, std::less<>> seen;
  std::size_t pos = 0;
  std::size_t records = 0;
  std::size_t bad_digests = 0;
  std::size_t undecoded = 0;
  while (pos < text.size()) {
    if ((++records & 63u) == 0) CVT_TRY(ctx.checkpoint(0.5 + 0.5 * static_cast<double>(pos) / static_cast<double>(text.size())));
    // Line breaks between records.
    while (pos < text.size() && (text[pos] == '\r' || text[pos] == '\n')) ++pos;
    if (pos >= text.size()) break;
    if (text.substr(pos, 7) != "WARC/1.") {
      if (records == 1) return fail(Status::Decode, "not a WARC file");
      return fail(Status::Decode, "WARC record at offset {} is damaged", pos);
    }
    const std::size_t version_end = text.find('\n', pos);
    if (version_end == std::string_view::npos) return fail(Status::Decode, "WARC record header is truncated");
    Fields fields;
    const auto consumed = parse_fields(text.substr(version_end + 1), fields);
    if (!consumed) return fail(Status::Decode, "WARC record header is truncated");
    const std::size_t block_at = version_end + 1 + *consumed;
    const auto length = parse_field(fields.get("Content-Length"), 10);
    if (!length) return fail(Status::Decode, "WARC record without a valid Content-Length");
    if (*length > text.size() - block_at) return fail(Status::Decode, "WARC record block is truncated");
    const ByteView block = data.subspan(block_at, static_cast<std::size_t>(*length));
    pos = block_at + static_cast<std::size_t>(*length);

    const std::string type = util::to_lower(fields.get("WARC-Type"));
    if (type != "response" && type != "resource") continue;
    if (const std::string_view digest = fields.get("WARC-Block-Digest"); util::istarts_with(digest, "sha1:")) {
      if (const auto expected = base32_sha1(digest.substr(5)); expected && *expected != util::sha1(block)) ++bad_digests;
    }
    const std::string_view uri = fields.get("WARC-Target-URI");
    std::string name = uri.empty() ? std::format("record-{}", records) : name_from_uri(uri);
    const std::string content_type = util::to_lower(fields.get("Content-Type"));
    const bool http = type == "response" && (content_type.find("application/http") != std::string::npos ||
                                             as_chars(block).starts_with("HTTP/"));
    ir::ArchiveEntry entry;
    if (http) {
      CVT_TRY_ASSIGN(Payload body, http_body(block, sink));
      if (!body.decoded_ok) ++undecoded;
      entry.bytes = std::move(body.bytes);
    } else {
      entry.bytes = to_bytes(block);
    }
    CVT_TRY(sink.charge(entry.bytes.size()));
    if (const auto found = seen.find(name); found != seen.end()) {
      std::string unique;
      do {
        unique = with_suffix(name, ++found->second);
      } while (seen.contains(unique));
      seen.emplace(unique, 1);
      name = std::move(unique);
    } else {
      seen.emplace(name, 1);
    }
    entry.path = std::move(name);
    entry.mtime = parse_iso_time(fields.get("WARC-Date")).value_or(0);
    entry.unix_mode = 0644;
    CVT_TRY(sink.add(std::move(entry)));
  }
  if (bad_digests != 0) ctx.warn(std::format("{} WARC record(s) do not match their WARC-Block-Digest.", bad_digests));
  if (undecoded != 0) {
    ctx.warn(std::format("{} HTTP response body(ies) could not be decoded and are kept as captured.", undecoded));
  }
  return sink.finish();
}

}  // namespace cvt::archive::unixfmt
