// Convertible core - document family: saved web pages: MHTML (.mht/.mhtml,
// MIME multipart/related) and Safari web archives (.webarchive, binary or
// XML property list). The main HTML part is read with the HTML reader;
// pictures stored in the archive are embedded.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <set>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::document {

namespace {

// =========================================================================== MIME

struct Part {
  std::map<std::string, std::string> headers;  // lower-case names
  std::string_view body;
};

std::string header_parameter(std::string_view value, std::string_view name) {
  const std::string lowered = util::to_lower(value);
  std::size_t at = 0;
  while ((at = lowered.find(name, at)) != std::string::npos) {
    const bool boundary_ok = at == 0 || lowered[at - 1] == ';' || lowered[at - 1] == ' ' || lowered[at - 1] == '\t';
    std::size_t i = at + name.size();
    while (i < lowered.size() && lowered[i] == ' ') ++i;
    if (!boundary_ok || i >= lowered.size() || lowered[i] != '=') {
      at += name.size();
      continue;
    }
    ++i;
    while (i < value.size() && value[i] == ' ') ++i;
    if (i < value.size() && value[i] == '"') {
      const std::size_t close = value.find('"', i + 1);
      return std::string(value.substr(i + 1, (close == std::string_view::npos ? value.size() : close) - i - 1));
    }
    std::size_t end = i;
    while (end < value.size() && value[end] != ';' && !util::is_ascii_space(value[end])) ++end;
    return std::string(value.substr(i, end - i));
  }
  return {};
}

// Headers up to the first empty line; returns the offset of the body.
std::size_t parse_headers(std::string_view text, std::map<std::string, std::string>& headers) {
  std::size_t i = 0;
  std::string name;
  while (i < text.size()) {
    std::size_t end = text.find('\n', i);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(i, end - i);
    if (line.ends_with('\r')) line.remove_suffix(1);
    i = end + 1;
    if (line.empty()) return std::min(i, text.size());
    if ((line.front() == ' ' || line.front() == '\t') && !name.empty()) {
      headers[name] += " " + std::string(util::trim(line));
      continue;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    name = util::to_lower(util::trim(line.substr(0, colon)));
    if (headers.size() < 200) headers[name] = std::string(util::trim(line.substr(colon + 1)));
  }
  return text.size();
}

std::vector<Part> split_multipart(std::string_view body, std::string_view boundary, std::size_t max_parts) {
  std::vector<Part> parts;
  if (boundary.empty()) return parts;
  const std::string delimiter = "--" + std::string(boundary);
  std::size_t at = body.find(delimiter);
  while (at != std::string_view::npos && parts.size() < max_parts) {
    std::size_t start = at + delimiter.size();
    if (body.substr(start).starts_with("--")) break;  // closing delimiter
    const std::size_t line_end = body.find('\n', start);
    if (line_end == std::string_view::npos) break;
    start = line_end + 1;
    std::size_t next = body.find(delimiter, start);
    std::size_t end = next == std::string_view::npos ? body.size() : next;
    // The line break before the delimiter belongs to it.
    if (end > start && body[end - 1] == '\n') --end;
    if (end > start && body[end - 1] == '\r') --end;
    Part part;
    const std::string_view raw = body.substr(start, end - start);
    const std::size_t offset = parse_headers(raw, part.headers);
    part.body = raw.substr(offset);
    parts.push_back(std::move(part));
    at = next;
  }
  return parts;
}

std::string quoted_printable(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '=') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r')) {
      ++i;
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      continue;
    }
    if (i + 2 < text.size()) {
      const auto value = util::parse_int(text.substr(i + 1, 2), 16);
      if (value) {
        out.push_back(static_cast<char>(*value));
        i += 2;
        continue;
      }
    }
    out.push_back('=');
  }
  return out;
}

Result<Bytes> decode_body(const Part& part, const Context& ctx) {
  const std::string encoding = util::to_lower(part.headers.contains("content-transfer-encoding")
                                                  ? part.headers.at("content-transfer-encoding")
                                                  : std::string());
  CVT_TRY(ctx.limits.check_decompressed(part.body.size()));
  if (encoding == "base64") {
    std::string cleaned;
    cleaned.reserve(part.body.size());
    for (const char c : part.body) {
      if (!util::is_ascii_space(c)) cleaned.push_back(c);
    }
    // Tolerate trailing garbage after the padding.
    auto decoded = util::base64_decode(cleaned);
    if (!decoded) return fail(Status::Decode, "MHTML part has invalid Base64 data");
    return std::move(*decoded);
  }
  if (encoding == "quoted-printable") return to_bytes(quoted_printable(part.body));
  return to_bytes(part.body);
}

std::string resolve_relative(std::string_view base, std::string_view url) {
  if (url.find("://") != std::string_view::npos || url.starts_with("cid:") || url.starts_with("data:")) return std::string(url);
  if (url.starts_with("//")) {
    const std::size_t colon = base.find(':');
    return std::string(base.substr(0, colon == std::string_view::npos ? 0 : colon + 1)) + std::string(url);
  }
  const std::size_t scheme = base.find("://");
  if (url.starts_with("/")) {
    if (scheme == std::string_view::npos) return std::string(url);
    const std::size_t path = base.find('/', scheme + 3);
    return std::string(base.substr(0, path)) + std::string(url);
  }
  const std::size_t slash = base.rfind('/');
  return std::string(base.substr(0, slash == std::string_view::npos ? 0 : slash + 1)) + std::string(url);
}

Result<ir::Document> read_mhtml(ByteView data, ReadContext& ctx) {
  const std::string_view text = as_chars(data);
  std::map<std::string, std::string> headers;
  const std::size_t offset = parse_headers(text, headers);
  const std::string content_type = headers["content-type"];
  std::vector<Part> parts;
  if (util::to_lower(content_type).starts_with("multipart/")) {
    parts = split_multipart(text.substr(offset), header_parameter(content_type, "boundary"),
                            static_cast<std::size_t>(std::min<std::uint64_t>(ctx.limits.max_archive_entries, 100000)));
  } else {
    Part single;
    single.headers = headers;
    single.body = text.substr(offset);
    parts.push_back(std::move(single));
  }
  if (parts.empty()) return fail(Status::Decode, "the MHTML file has no parts (missing boundary?)");
  // Main part: the multipart "start" or the first text/html part.
  const std::string start = header_parameter(content_type, "start");
  const Part* main = nullptr;
  for (const Part& p : parts) {
    const std::string type = util::to_lower(p.headers.contains("content-type") ? p.headers.at("content-type") : "text/plain");
    if (!start.empty() && p.headers.contains("content-id") && p.headers.at("content-id") == start) {
      main = &p;
      break;
    }
    if (main == nullptr && type.starts_with("text/html")) main = &p;
  }
  if (main == nullptr) main = &parts.front();
  // Resources by location and content id.
  std::map<std::string, const Part*> resources;
  for (const Part& p : parts) {
    if (p.headers.contains("content-location")) resources[p.headers.at("content-location")] = &p;
    if (p.headers.contains("content-id")) {
      std::string id = p.headers.at("content-id");
      if (id.starts_with("<") && id.ends_with(">")) id = id.substr(1, id.size() - 2);
      resources["cid:" + id] = &p;
    }
  }
  const std::string base = main->headers.contains("content-location") ? main->headers.at("content-location") : std::string();
  CVT_TRY_ASSIGN(Bytes html_bytes, decode_body(*main, ctx));
  std::vector<std::string> warnings;
  HtmlReadOptions options;
  const std::string charset = header_parameter(main->headers.contains("content-type") ? main->headers.at("content-type") : "", "charset");
  if (!charset.empty() && is_known_charset(charset)) options.forced_encoding = charset;
  std::uint64_t total = html_bytes.size();
  bool over_limit = false;
  options.resolve = [&](std::string_view url) -> std::optional<ir::EncodedImage> {
    auto it = resources.find(std::string(url));
    if (it == resources.end()) it = resources.find(resolve_relative(base, url));
    if (it == resources.end()) return std::nullopt;
    auto bytes = decode_body(*it->second, ctx);
    if (!bytes) return std::nullopt;
    total += bytes->size();
    if (!ctx.limits.check_decompressed(total)) {
      over_limit = true;
      return std::nullopt;
    }
    ir::EncodedImage image;
    image.bytes = std::move(*bytes);
    const std::string type = it->second->headers.contains("content-type") ? it->second->headers.at("content-type") : std::string();
    image.mime = util::to_lower(type.substr(0, type.find(';')));
    if (image.mime.empty() || !image.mime.starts_with("image/")) image.mime = sniff_image_mime(image.bytes);
    return image;
  };
  auto document = parse_html(html_bytes, options, &warnings);
  for (std::string& w : warnings) ctx.warn(std::move(w));
  if (over_limit) ctx.warn("Some pictures were left out because the archive is too large.");
  if (document && document->metadata.title.empty() && headers.contains("subject")) document->metadata.title = headers["subject"];
  return document;
}

int sniff_mhtml(ByteView data, std::string_view filename) {
  const std::string lowered = util::to_lower(sniff_head(data, 4096));
  const bool mime = lowered.find("mime-version:") != std::string::npos && lowered.find("multipart/related") != std::string::npos;
  if (mime) return has_extension(filename, {"mht", "mhtml"}) ? 95 : 86;
  return has_extension(filename, {"mht", "mhtml"}) && lowered.find("content-type:") != std::string::npos ? 75 : 0;
}

// =========================================================================== property lists

struct PlistValue {
  enum class Type : std::uint8_t { Null, Bool, Int, Real, Data, String, Array, Dict } type = Type::Null;
  std::int64_t integer = 0;
  Bytes data;
  std::string string;
  std::vector<PlistValue> array;
  std::vector<std::pair<std::string, PlistValue>> dict;

  [[nodiscard]] const PlistValue* get(std::string_view key) const {
    for (const auto& [k, v] : dict) {
      if (k == key) return &v;
    }
    return nullptr;
  }
};

class BinaryPlist {
 public:
  BinaryPlist(ByteView data, const Context& ctx) : data_(data), ctx_(ctx) {}

  Result<PlistValue> parse() {
    if (data_.size() < 40 || !as_chars(data_).starts_with("bplist00")) return fail(Status::Decode, "not a binary property list");
    const ByteView trailer = data_.last(32);
    offset_size_ = std::to_integer<std::uint8_t>(trailer[6]);
    ref_size_ = std::to_integer<std::uint8_t>(trailer[7]);
    util::ByteReader r(trailer.subspan(8));
    count_ = r.u64be();
    const std::uint64_t top = r.u64be();
    table_ = r.u64be();
    if (offset_size_ < 1 || offset_size_ > 8 || ref_size_ < 1 || ref_size_ > 8) return fail(Status::Decode, "corrupt property list trailer");
    CVT_TRY(ctx_.limits.check_archive_entries(count_));
    if (count_ == 0 || top >= count_ || table_ >= data_.size() || count_ > (data_.size() - table_) / offset_size_) {
      return fail(Status::Decode, "corrupt property list offsets");
    }
    std::vector<bool> visiting(static_cast<std::size_t>(count_), false);
    visiting_ = std::move(visiting);
    return object(top, 0);
  }

 private:
  std::uint64_t read_uint(std::uint64_t at, unsigned size) const {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | std::to_integer<std::uint8_t>(data_[static_cast<std::size_t>(at + i)]);
    return value;
  }

  Result<PlistValue> object(std::uint64_t index, int depth) {
    if (depth > 64) return fail(Status::Limit, "property list nested too deeply");
    if (index >= count_) return fail(Status::Decode, "property list reference out of range");
    if (visiting_[static_cast<std::size_t>(index)]) return fail(Status::Decode, "property list contains a cycle");
    if (++objects_ > 1'000'000) return fail(Status::Limit, "property list has too many objects");
    const std::uint64_t offset = read_uint(table_ + index * offset_size_, offset_size_);
    if (offset >= table_) return fail(Status::Decode, "property list object offset out of range");
    visiting_[static_cast<std::size_t>(index)] = true;
    auto result = decode(offset, depth);
    visiting_[static_cast<std::size_t>(index)] = false;
    return result;
  }

  // Length field: low nibble, or 0xF followed by an int object.
  Result<std::pair<std::uint64_t, std::uint64_t>> length(std::uint64_t at, std::uint8_t marker) {
    std::uint64_t n = marker & 0x0F;
    std::uint64_t pos = at + 1;
    if (n == 0x0F) {
      if (pos >= table_) return fail(Status::Decode, "truncated property list");
      const std::uint8_t int_marker = std::to_integer<std::uint8_t>(data_[static_cast<std::size_t>(pos)]);
      if ((int_marker & 0xF0) != 0x10) return fail(Status::Decode, "corrupt property list length");
      const unsigned size = 1u << (int_marker & 0x0F);
      if (size > 8 || pos + 1 + size > table_) return fail(Status::Decode, "corrupt property list length");
      n = read_uint(pos + 1, size);
      pos += 1 + size;
    }
    return std::make_pair(n, pos);
  }

  Result<PlistValue> decode(std::uint64_t at, int depth) {
    PlistValue value;
    const std::uint8_t marker = std::to_integer<std::uint8_t>(data_[static_cast<std::size_t>(at)]);
    const std::uint8_t kind = marker >> 4;
    switch (kind) {
      case 0x0:
        value.type = marker == 0x08 || marker == 0x09 ? PlistValue::Type::Bool : PlistValue::Type::Null;
        value.integer = marker == 0x09 ? 1 : 0;
        return value;
      case 0x1: {
        const unsigned size = 1u << (marker & 0x0F);
        if (size > 8 || at + 1 + size > table_) return fail(Status::Decode, "truncated property list integer");
        value.type = PlistValue::Type::Int;
        value.integer = static_cast<std::int64_t>(read_uint(at + 1, size));
        return value;
      }
      case 0x2:
      case 0x3:
        value.type = PlistValue::Type::Real;
        return value;
      case 0x4:
      case 0x5:
      case 0x6: {
        CVT_TRY_ASSIGN(const auto len, length(at, marker));
        const std::uint64_t bytes = kind == 0x6 ? len.first * 2 : len.first;
        if (len.first > table_ || bytes > table_ - len.second) return fail(Status::Decode, "truncated property list data");
        CVT_TRY(ctx_.limits.check_decompressed(bytes));
        const ByteView raw = data_.subspan(static_cast<std::size_t>(len.second), static_cast<std::size_t>(bytes));
        if (kind == 0x4) {
          value.type = PlistValue::Type::Data;
          value.data = to_bytes(raw);
        } else if (kind == 0x5) {
          value.type = PlistValue::Type::String;
          value.string = util::decode_text(raw, util::TextEncoding::Windows1252).text;
        } else {
          value.type = PlistValue::Type::String;
          value.string = util::decode_text(raw, util::TextEncoding::Utf16BE).text;
        }
        return value;
      }
      case 0x8: {
        CVT_TRY_ASSIGN(const auto len, length(at, marker));
        value.type = PlistValue::Type::String;  // UID, not needed
        (void)len;
        return value;
      }
      case 0xA:
      case 0xD: {
        CVT_TRY_ASSIGN(const auto len, length(at, marker));
        const std::uint64_t refs = kind == 0xD ? len.first * 2 : len.first;
        if (len.first > count_ || refs > (table_ - len.second) / ref_size_) return fail(Status::Decode, "truncated property list container");
        if (kind == 0xA) {
          value.type = PlistValue::Type::Array;
          for (std::uint64_t i = 0; i < len.first; ++i) {
            CVT_TRY_ASSIGN(PlistValue element, object(read_uint(len.second + i * ref_size_, ref_size_), depth + 1));
            value.array.push_back(std::move(element));
          }
        } else {
          value.type = PlistValue::Type::Dict;
          for (std::uint64_t i = 0; i < len.first; ++i) {
            CVT_TRY_ASSIGN(PlistValue key, object(read_uint(len.second + i * ref_size_, ref_size_), depth + 1));
            CVT_TRY_ASSIGN(PlistValue element, object(read_uint(len.second + (len.first + i) * ref_size_, ref_size_), depth + 1));
            value.dict.emplace_back(std::move(key.string), std::move(element));
          }
        }
        return value;
      }
      default: return value;
    }
  }

  ByteView data_;
  const Context& ctx_;
  unsigned offset_size_ = 0;
  unsigned ref_size_ = 0;
  std::uint64_t count_ = 0;
  std::uint64_t table_ = 0;
  std::vector<bool> visiting_;
  std::uint64_t objects_ = 0;
};

Result<PlistValue> xml_plist_value(util::XmlNode node, int depth, const Context& ctx) {
  if (depth > 64) return fail(Status::Limit, "property list nested too deeply");
  PlistValue value;
  const std::string_view name = node.local_name();
  if (name == "dict") {
    value.type = PlistValue::Type::Dict;
    std::string key;
    for (util::XmlNode child : node.elements()) {
      if (child.local_name() == "key") {
        key = child.text();
        continue;
      }
      CVT_TRY_ASSIGN(PlistValue element, xml_plist_value(child, depth + 1, ctx));
      value.dict.emplace_back(key, std::move(element));
    }
  } else if (name == "array") {
    value.type = PlistValue::Type::Array;
    for (util::XmlNode child : node.elements()) {
      CVT_TRY_ASSIGN(PlistValue element, xml_plist_value(child, depth + 1, ctx));
      value.array.push_back(std::move(element));
    }
  } else if (name == "data") {
    std::string cleaned;
    for (const char c : node.text()) {
      if (!util::is_ascii_space(c)) cleaned.push_back(c);
    }
    CVT_TRY(ctx.limits.check_decompressed(cleaned.size()));
    auto bytes = util::base64_decode(cleaned);
    value.type = PlistValue::Type::Data;
    if (bytes) value.data = std::move(*bytes);
  } else {
    value.type = PlistValue::Type::String;
    value.string = node.text();
  }
  return value;
}

Result<ir::Document> read_webarchive(ByteView data, ReadContext& ctx) {
  PlistValue root;
  if (as_chars(data).starts_with("bplist00")) {
    BinaryPlist plist(data, ctx);
    CVT_TRY_ASSIGN(root, plist.parse());
  } else {
    auto xml = util::parse_xml(data);
    if (!xml) return fail(Status::Decode, "not a web archive property list");
    util::XmlNode top = xml->root().child_local("dict");
    if (!top) return fail(Status::Decode, "not a web archive property list");
    CVT_TRY_ASSIGN(root, xml_plist_value(top, 0, ctx));
  }
  const PlistValue* main = root.get("WebMainResource");
  if (main == nullptr || main->get("WebResourceData") == nullptr) return fail(Status::Decode, "the web archive has no main resource");
  std::map<std::string, const PlistValue*> resources;
  const auto collect = [&](const PlistValue* list) {
    if (list == nullptr) return;
    for (const PlistValue& r : list->array) {
      if (const PlistValue* url = r.get("WebResourceURL")) resources[url->string] = &r;
    }
  };
  collect(root.get("WebSubresources"));
  std::string base;
  if (const PlistValue* url = main->get("WebResourceURL")) base = url->string;
  HtmlReadOptions options;
  if (const PlistValue* encoding = main->get("WebResourceTextEncodingName"); encoding != nullptr && is_known_charset(encoding->string)) {
    options.forced_encoding = encoding->string;
  }
  options.resolve = [&](std::string_view url) -> std::optional<ir::EncodedImage> {
    auto it = resources.find(std::string(url));
    if (it == resources.end()) it = resources.find(resolve_relative(base, url));
    if (it == resources.end()) return std::nullopt;
    const PlistValue* bytes = it->second->get("WebResourceData");
    if (bytes == nullptr) return std::nullopt;
    ir::EncodedImage image;
    image.bytes = bytes->data;
    if (const PlistValue* mime = it->second->get("WebResourceMIMEType")) image.mime = mime->string;
    if (!image.mime.starts_with("image/")) image.mime = sniff_image_mime(image.bytes);
    return image;
  };
  std::vector<std::string> warnings;
  auto document = parse_html(main->get("WebResourceData")->data, options, &warnings);
  for (std::string& w : warnings) ctx.warn(std::move(w));
  return document;
}

int sniff_webarchive(ByteView data, std::string_view filename) {
  const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 65536)));
  if (head.starts_with("bplist00") && head.find("WebMainResource") != std::string_view::npos) return 99;
  if (head.find("<plist") != std::string_view::npos && head.find("WebMainResource") != std::string_view::npos) return 97;
  if (has_extension(filename, {"webarchive"}) && head.starts_with("bplist00")) return 85;
  return 0;
}

}  // namespace

void register_web_archives(Registry& registry) {
  MarkupFormat mhtml;
  mhtml.id = "mhtml";
  mhtml.name = "MHTML web archive";
  mhtml.description = "Web page saved with its pictures as a MIME multipart file (read).";
  mhtml.extensions = {"mht", "mhtml"};
  mhtml.mime = "multipart/related";
  mhtml.popularity = 30;
  mhtml.sniff = sniff_mhtml;
  mhtml.read_bytes = read_mhtml;
  register_markup(registry, std::move(mhtml));

  MarkupFormat webarchive;
  webarchive.id = "webarchive";
  webarchive.name = "Safari web archive";
  webarchive.description = "Web page saved by Safari with its resources (read).";
  webarchive.extensions = {"webarchive"};
  webarchive.mime = "application/x-webarchive";
  webarchive.popularity = 20;
  webarchive.sniff = sniff_webarchive;
  webarchive.read_bytes = read_webarchive;
  register_markup(registry, std::move(webarchive));
}

}  // namespace cvt::document
