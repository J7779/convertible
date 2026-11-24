// Convertible core - office family: Kindle and Palm e-books.
//
// Reader: MOBI / PRC / AZW (Mobipocket 6: PalmDOC or HUFF/CDIC text,
// filepos links, recindex pictures, EXTH metadata) and KF8 / AZW3 (also
// the KF8 half of combined files): FDST flows, skeleton and fragment
// indexes (INDX/TAGX) reassembled into the original XHTML files,
// kindle:embed pictures. Books with DRM are refused with a clear message.
// Plain Palm e-books (PalmDOC "TEXtREAd", zTXT-less) read as text.
//
// Writer: classic MOBI 6 (PalmDOC compressed UTF-8 HTML, pictures as
// records, EXTH title/language, multibyte overlap trailing entries).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/epub.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "families/office/palm.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

constexpr std::uint32_t null_index = 0xFFFFFFFFu;

std::uint32_t be32(ByteView data, std::size_t at) {
  util::ByteReader r(data);
  if (!r.seek(at)) return null_index;
  const std::uint32_t v = r.u32be();
  return r.ok() ? v : null_index;
}

std::uint16_t be16(ByteView data, std::size_t at) {
  util::ByteReader r(data);
  if (!r.seek(at)) return 0;
  const std::uint16_t v = r.u16be();
  return r.ok() ? v : 0;
}

// --------------------------------------------------------------------------- INDX

// Forward variable-width integer: 7 bits per byte, the last byte has bit 7 set.
std::uint32_t decint(util::ByteReader& r) {
  std::uint32_t value = 0;
  for (int i = 0; i < 5 && !r.at_end(); ++i) {
    const std::uint8_t b = r.u8();
    value = (value << 7) | (b & 0x7F);
    if (b & 0x80) break;
  }
  return value;
}

struct IndexEntry {
  std::string key;
  std::map<std::uint32_t, std::vector<std::uint32_t>> tags;
};

Result<std::vector<IndexEntry>> read_index(const palm::Database& db, std::size_t first) {
  if (first >= db.records.size()) return fail(Status::Decode, "the index record is missing");
  const ByteView header = db.records[first];
  if (header.size() < 64 || as_chars(header).substr(0, 4) != "INDX") return fail(Status::Decode, "bad INDX record");
  const std::uint32_t header_length = be32(header, 4);
  const std::uint32_t count = be32(header, 24);
  if (count > 4096 || header_length >= header.size()) return fail(Status::Decode, "bad INDX header");
  // TAGX section.
  util::ByteReader t(header);
  t.seek(header_length);
  if (t.chars(4) != "TAGX") return fail(Status::Decode, "the index has no TAGX section");
  const std::uint32_t tagx_length = t.u32be();
  const std::uint32_t control_bytes = t.u32be();
  struct Tag {
    std::uint8_t tag, per_entry, mask, end;
  };
  std::vector<Tag> tagx;
  if (tagx_length < 12 || tagx_length > 4096) return fail(Status::Decode, "bad TAGX section");
  for (std::uint32_t i = 12; i + 4 <= tagx_length; i += 4) {
    Tag tag{t.u8(), t.u8(), t.u8(), t.u8()};
    tagx.push_back(tag);
  }
  if (!t.ok() || control_bytes > 8) return fail(Status::Decode, "bad TAGX section");

  std::vector<IndexEntry> entries;
  for (std::uint32_t rec = 1; rec <= count; ++rec) {
    if (first + rec >= db.records.size()) break;
    const ByteView data = db.records[first + rec];
    if (data.size() < 28 || as_chars(data).substr(0, 4) != "INDX") continue;
    const std::uint32_t idxt = be32(data, 20);
    const std::uint32_t entry_count = be32(data, 24);
    if (idxt >= data.size() || entry_count > 65535) continue;
    std::vector<std::uint32_t> offsets;
    util::ByteReader ir(data);
    ir.seek(idxt);
    if (ir.chars(4) != "IDXT") continue;
    for (std::uint32_t i = 0; i < entry_count; ++i) offsets.push_back(ir.u16be());
    offsets.push_back(idxt);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
      const std::uint32_t start = offsets[i];
      const std::uint32_t end = offsets[i + 1];
      if (start >= end || end > data.size()) continue;
      util::ByteReader e(data.subspan(start, end - start));
      IndexEntry entry;
      const std::uint8_t length = e.u8();
      entry.key = std::string(e.chars(length));
      std::vector<std::uint8_t> controls;
      for (std::uint32_t k = 0; k < control_bytes; ++k) controls.push_back(e.u8());
      if (!e.ok()) continue;
      struct Pending {
        std::uint8_t tag;
        std::uint32_t count;  // number of values (0 = byte length given)
        std::uint32_t bytes;
        std::uint8_t per_entry;
      };
      std::vector<Pending> pending;
      std::size_t control_index = 0;
      for (const Tag& tag : tagx) {
        if (tag.end) {
          ++control_index;
          continue;
        }
        if (control_index >= controls.size() || tag.mask == 0) break;
        std::uint32_t value = controls[control_index] & tag.mask;
        if (value == 0) continue;
        std::uint8_t mask = tag.mask;
        if (value == mask) {
          if (std::popcount(mask) > 1) {
            pending.push_back(Pending{tag.tag, 0, decint(e), tag.per_entry});
          } else {
            pending.push_back(Pending{tag.tag, 1, 0, tag.per_entry});
          }
        } else {
          while ((mask & 1) == 0) {
            mask >>= 1;
            value >>= 1;
          }
          pending.push_back(Pending{tag.tag, value, 0, tag.per_entry});
        }
      }
      for (const Pending& p : pending) {
        std::vector<std::uint32_t>& values = entry.tags[p.tag];
        if (p.count > 0) {
          const std::uint64_t n = std::uint64_t{p.count} * p.per_entry;
          for (std::uint64_t k = 0; k < n && k < 64 && !e.at_end(); ++k) values.push_back(decint(e));
        } else {
          const std::size_t stop = std::min<std::size_t>(e.position() + p.bytes, e.size());
          while (e.position() < stop) values.push_back(decint(e));
        }
      }
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

// --------------------------------------------------------------------------- reader

struct Header {
  std::size_t base = 0;  // index of the header record
  std::uint16_t compression = 1;
  std::uint32_t text_length = 0;
  std::uint16_t text_records = 0;
  std::uint16_t encryption = 0;
  std::uint32_t encoding = 1252;
  std::uint32_t version = 0;
  std::uint32_t header_length = 0;
  std::uint32_t first_image = null_index;
  std::uint32_t huff_offset = 0;
  std::uint32_t huff_count = 0;
  std::uint32_t extra_flags = 0;
  std::uint32_t fdst = null_index;
  std::uint32_t skeleton = null_index;
  std::uint32_t fragment = null_index;
  std::uint32_t kf8_boundary = null_index;
  std::string full_name;
  std::map<std::uint32_t, std::string> exth;
};

Result<Header> parse_header(const palm::Database& db, std::size_t base) {
  Header h;
  h.base = base;
  if (base >= db.records.size()) return fail(Status::Decode, "the book header record is missing");
  const ByteView r0 = db.records[base];
  if (r0.size() < 16) return fail(Status::Decode, "the book header is truncated");
  h.compression = be16(r0, 0);
  h.text_length = be32(r0, 4);
  h.text_records = be16(r0, 8);
  h.encryption = be16(r0, 12);
  if (r0.size() >= 24 && as_chars(r0).substr(16, 4) == "MOBI") {
    h.header_length = be32(r0, 20);
    h.encoding = be32(r0, 28);
    h.version = be32(r0, 36);
    const std::uint32_t name_offset = be32(r0, 84);
    const std::uint32_t name_length = be32(r0, 88);
    if (name_offset < r0.size() && name_length < r0.size() - name_offset) {
      h.full_name = std::string(as_chars(r0).substr(name_offset, name_length));
    }
    h.first_image = be32(r0, 108);
    h.huff_offset = be32(r0, 112);
    h.huff_count = be32(r0, 116);
    const std::uint32_t exth_flags = be32(r0, 128);
    if (h.header_length >= 0xE4 && r0.size() >= 244) h.extra_flags = be16(r0, 242);
    if (h.version >= 8) {
      h.fdst = be32(r0, 192);
      h.fragment = be32(r0, 248);
      h.skeleton = be32(r0, 252);
    }
    if (exth_flags & 0x40) {
      const std::size_t at = 16 + static_cast<std::size_t>(h.header_length);
      util::ByteReader e(r0);
      if (e.seek(at) && e.chars(4) == "EXTH") {
        e.skip(4);
        const std::uint32_t count = e.u32be();
        for (std::uint32_t i = 0; i < count && i < 1024 && e.ok(); ++i) {
          const std::uint32_t type = e.u32be();
          const std::uint32_t length = e.u32be();
          if (length < 8 || length - 8 > e.remaining()) break;
          const std::string_view value = e.chars(length - 8);
          if (type == 121 && value.size() == 4) {
            h.kf8_boundary = be32(as_bytes(value), 0);
          } else {
            h.exth.emplace(type, std::string(value));
          }
        }
      }
    }
  }
  return h;
}

Result<Bytes> book_text(const palm::Database& db, const Header& h, const Limits& limits, const Context& ctx) {
  if (h.encryption != 0) {
    return fail(Status::Decode, "this e-book is protected by DRM; it cannot be converted");
  }
  const std::uint64_t cap = std::min<std::uint64_t>(limits.max_decompressed_bytes, 512ull * 1024 * 1024);
  Bytes text;
  std::optional<palm::HuffCdic> huff;
  if (h.compression == 17480) {
    const std::size_t at = h.base + h.huff_offset;
    if (h.huff_count == 0 || at >= db.records.size() || h.huff_count > 256) return fail(Status::Decode, "the HUFF records are missing");
    std::vector<ByteView> cdics;
    for (std::uint32_t i = 1; i < h.huff_count && at + i < db.records.size(); ++i) cdics.push_back(db.records[at + i]);
    huff.emplace();
    CVT_TRY(huff->load(db.records[at], cdics));
  } else if (h.compression != 1 && h.compression != 2) {
    return fail(Status::Decode, "unknown text compression {}", h.compression);
  }
  for (std::uint32_t i = 1; i <= h.text_records; ++i) {
    if ((i & 31) == 0) CVT_TRY(ctx.checkpoint(0.5 * i / h.text_records, "Decompressing"));
    if (h.base + i >= db.records.size()) break;
    ByteView record = db.records[h.base + i];
    record = record.first(record.size() - palm::trailing_size(record, h.extra_flags));
    if (h.compression == 1) {
      text.insert(text.end(), record.begin(), record.end());
      if (text.size() > cap) return fail(Status::Limit, "the book text exceeds the limit");
    } else if (h.compression == 2) {
      CVT_TRY(palm::palmdoc_decompress(record, text, cap));
    } else {
      CVT_TRY(huff->decompress(record, text, cap));
    }
  }
  if (h.text_length < text.size()) text.resize(h.text_length);
  return text;
}

std::string decode_book_text(const Bytes& text, std::uint32_t encoding) {
  if (encoding == 65001) return util::sanitize_utf8(as_chars(text));
  return util::decode_text(text, util::TextEncoding::Windows1252).text;
}

std::optional<ir::EncodedImage> image_record(const palm::Database& db, std::size_t index) {
  if (index >= db.records.size()) return std::nullopt;
  const ByteView data = db.records[index];
  const std::string mime = document::sniff_image_mime(data);
  if (mime.empty()) return std::nullopt;
  ir::EncodedImage image;
  image.bytes = to_bytes(data);
  image.mime = mime;
  return image;
}

// MOBI 6 markup: page breaks, filepos anchors and recindex pictures.
std::string prepare_mobi6_html(std::string html) {
  html = util::replace_all(html, "<mbp:pagebreak/>", "<div class=\"page-break\"></div>");
  html = util::replace_all(html, "<mbp:pagebreak />", "<div class=\"page-break\"></div>");
  html = util::replace_all(html, "<mbp:pagebreak>", "<div class=\"page-break\"></div>");
  // filepos values are byte offsets into this text: collect them first.
  const auto parse_filepos = [&](std::size_t at, std::size_t& end) -> std::optional<std::size_t> {
    std::size_t j = at + 8;
    const bool quoted = j < html.size() && (html[j] == '"' || html[j] == '\'');
    if (quoted) ++j;
    std::size_t k = j;
    while (k < html.size() && util::is_ascii_digit(html[k]) && k - j < 12) ++k;
    const auto value = util::parse_int(std::string_view(html).substr(j, k - j));
    if (quoted && k < html.size() && (html[k] == '"' || html[k] == '\'')) ++k;
    end = k;
    if (!value || *value < 0) return std::nullopt;
    return static_cast<std::size_t>(*value);
  };
  std::vector<std::size_t> targets;
  for (std::size_t at = html.find("filepos="); at != std::string::npos; at = html.find("filepos=", at + 8)) {
    std::size_t end = 0;
    if (auto value = parse_filepos(at, end)) targets.push_back(*value);
    if (targets.size() > 100000) break;
  }
  std::ranges::sort(targets);
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  std::string out;
  out.reserve(html.size() + targets.size() * 32);
  bool in_tag = false;
  std::size_t tag_start = 0;
  std::size_t t = 0;
  std::size_t p = 0;
  while (true) {
    while (t < targets.size() && targets[t] <= p) {
      const std::string anchor = "<a id=\"filepos" + std::to_string(targets[t]) + "\"></a>";
      if (in_tag) {
        out.insert(tag_start, anchor);
        tag_start += anchor.size();
      } else {
        out += anchor;
      }
      ++t;
    }
    if (p >= html.size()) break;
    const char c = html[p];
    if (c == '<') {
      in_tag = true;
      tag_start = out.size();
    }
    if (in_tag && html.compare(p, 8, "filepos=") == 0) {
      std::size_t end = p + 8;
      if (auto value = parse_filepos(p, end)) out += "href=\"#filepos" + std::to_string(*value) + "\"";
      p = std::max(end, p + 8);
      continue;
    }
    out.push_back(c);
    if (c == '>') in_tag = false;
    ++p;
  }
  out = util::replace_all(out, "recindex=\"", "src=\"recindex:");
  out = util::replace_all(out, "recindex='", "src='recindex:");
  return out;
}

std::uint32_t base32_value(std::string_view text) {
  std::uint32_t value = 0;
  for (char c : text) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'A' && c <= 'V') digit = c - 'A' + 10;
    else if (c >= 'a' && c <= 'v') digit = c - 'a' + 10;
    if (digit < 0) break;
    value = value * 32 + static_cast<std::uint32_t>(digit);
    if (value > 1'000'000) break;
  }
  return value;
}

Result<std::vector<std::string>> kf8_parts(const palm::Database& db, const Header& h, const std::string& text) {
  // Flow 0 is the XHTML text; the others hold CSS and SVG.
  std::string flow = text;
  if (h.fdst != null_index && h.base + h.fdst < db.records.size()) {
    const ByteView fdst = db.records[h.base + h.fdst];
    if (fdst.size() >= 20 && as_chars(fdst).substr(0, 4) == "FDST") {
      const std::uint32_t start = be32(fdst, 12);
      const std::uint32_t end = be32(fdst, 16);
      if (start <= end && end <= text.size()) flow = text.substr(start, end - start);
    }
  }
  if (h.skeleton == null_index || h.fragment == null_index) return std::vector<std::string>{flow};
  CVT_TRY_ASSIGN(const std::vector<IndexEntry> skeletons, read_index(db, h.base + h.skeleton));
  CVT_TRY_ASSIGN(const std::vector<IndexEntry> fragments, read_index(db, h.base + h.fragment));
  std::vector<std::string> parts;
  std::size_t fragment = 0;
  for (const IndexEntry& skeleton : skeletons) {
    const auto count = skeleton.tags.find(1);
    const auto span = skeleton.tags.find(6);
    if (count == skeleton.tags.end() || span == skeleton.tags.end() || count->second.empty() || span->second.size() < 2) {
      continue;
    }
    const std::uint64_t start = span->second[0];
    const std::uint64_t length = span->second[1];
    if (start + length > flow.size()) return fail(Status::Decode, "a KF8 skeleton lies outside the text");
    std::string part = flow.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(length));
    std::uint64_t base = start + length;
    for (std::uint32_t k = 0; k < count->second[0] && fragment < fragments.size(); ++k, ++fragment) {
      const IndexEntry& f = fragments[fragment];
      const auto fspan = f.tags.find(6);
      const auto insert = util::parse_int(f.key);
      if (fspan == f.tags.end() || fspan->second.size() < 2 || !insert) continue;
      const std::uint64_t flen = fspan->second[1];
      if (base + flen > flow.size()) return fail(Status::Decode, "a KF8 fragment lies outside the text");
      const std::int64_t at = *insert - static_cast<std::int64_t>(start);
      const std::size_t pos = static_cast<std::size_t>(std::clamp<std::int64_t>(at, 0, static_cast<std::int64_t>(part.size())));
      part.insert(pos, flow, static_cast<std::size_t>(base), static_cast<std::size_t>(flen));
      base += flen;
    }
    parts.push_back(std::move(part));
  }
  if (parts.empty()) parts.push_back(flow);
  return parts;
}

void apply_exth(ir::Document& doc, const Header& h) {
  const auto get = [&](std::uint32_t type) -> std::string {
    const auto it = h.exth.find(type);
    return it == h.exth.end() ? std::string() : util::sanitize_utf8(it->second);
  };
  doc.metadata.title = get(503);
  if (doc.metadata.title.empty()) doc.metadata.title = util::sanitize_utf8(h.full_name);
  doc.metadata.author = get(100);
  doc.metadata.language = get(524);
  doc.metadata.date = get(106);
  if (const std::string p = get(101); !p.empty()) doc.metadata.extra["publisher"] = p;
  if (const std::string d = get(103); !d.empty()) doc.metadata.extra["description"] = d;
}

Result<AnyIR> read_mobi(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const palm::Database db, palm::open(ctx.input, ctx.limits));
  const ReadSettings settings = read_settings(ctx.options);
  if (db.type == "TEXt" && db.creator == "REAd") {
    // A plain PalmDOC book.
    CVT_TRY_ASSIGN(const Header h, parse_header(db, 0));
    CVT_TRY_ASSIGN(Bytes text, book_text(db, h, ctx.limits, ctx));
    ir::Document doc = document::infer_document_from_text(decode_book_text(text, 1252));
    doc.metadata.title = util::sanitize_utf8(db.name);
    finish_metadata(doc, settings);
    return AnyIR(std::move(doc));
  }
  if (db.type != "BOOK" || db.creator != "MOBI") {
    if (db.type == "PNRd" || db.creator == "PPrs") {
      return fail(Status::Decode, "eReader books are not supported");
    }
    return fail(Status::Decode, "not a Mobipocket or Kindle book ({}/{})", db.type, db.creator);
  }
  CVT_TRY_ASSIGN(Header header, parse_header(db, 0));
  if (header.encryption != 0) return fail(Status::Decode, "this e-book is protected by DRM; it cannot be converted");
  ir::Document doc;

  // Prefer the KF8 half (newer, more faithful) when the book has one.
  std::optional<Header> kf8;
  if (header.version >= 8) {
    kf8 = header;
  } else if (header.kf8_boundary != null_index && header.kf8_boundary < db.records.size()) {
    auto h = parse_header(db, header.kf8_boundary);
    if (h && h->version >= 8) kf8 = std::move(*h);
  }
  bool done = false;
  if (kf8) {
    auto text = book_text(db, *kf8, ctx.limits, ctx);
    if (text) {
      const std::string decoded = decode_book_text(*text, kf8->encoding);
      auto parts = kf8_parts(db, *kf8, decoded);
      if (parts) {
        document::HtmlReadOptions options;
        options.read_metadata = false;
        const std::size_t image_base = kf8->first_image == null_index ? db.records.size() : kf8->base + kf8->first_image;
        options.resolve = [&db, image_base](std::string_view url) -> std::optional<ir::EncodedImage> {
          if (!url.starts_with("kindle:embed:")) return std::nullopt;
          const std::uint32_t n = base32_value(url.substr(13));
          if (n == 0) return std::nullopt;
          return image_record(db, image_base + n - 1);
        };
        for (std::size_t i = 0; i < parts->size(); ++i) {
          CVT_TRY(ctx.checkpoint(0.5 + 0.5 * static_cast<double>(i) / static_cast<double>(parts->size()), "Reading"));
          CVT_TRY_ASSIGN(ir::Document part, document::parse_html_text((*parts)[i], options));
          if (!doc.blocks.empty() && !part.blocks.empty() && ctx.options.get_bool("chapter_breaks", true)) {
            doc.blocks.push_back(ir::PageBreak{});
          }
          for (ir::Block& b : part.blocks) doc.blocks.push_back(std::move(b));
          for (ir::Footnote& n : part.footnotes) doc.footnotes.push_back(std::move(n));
        }
        apply_exth(doc, *kf8);
        done = true;
      }
    }
    if (!done && header.version >= 8) {
      if (!text) return fail(text.error());
    }
  }
  if (!done) {
    CVT_TRY_ASSIGN(Bytes text, book_text(db, header, ctx.limits, ctx));
    const std::string html = prepare_mobi6_html(decode_book_text(text, header.encoding));
    document::HtmlReadOptions options;
    options.read_metadata = false;
    const std::size_t image_base = header.first_image == null_index ? db.records.size() : header.first_image;
    options.resolve = [&db, image_base](std::string_view url) -> std::optional<ir::EncodedImage> {
      if (!url.starts_with("recindex:")) return std::nullopt;
      const auto n = util::parse_int(url.substr(9));
      if (!n || *n < 1) return std::nullopt;
      return image_record(db, image_base + static_cast<std::size_t>(*n) - 1);
    };
    CVT_TRY_ASSIGN(doc, document::parse_html_text(html, options));
    apply_exth(doc, header);
  }
  // Kindle-internal position links do not survive outside the book.
  // (Links to "#fileposN" anchors do.)
  finish_metadata(doc, settings);
  return AnyIR(std::move(doc));
}

// --------------------------------------------------------------------------- writer

Result<Bytes> write_mobi(const AnyIR& value, WriteContext& ctx) {
  ir::Document doc = value.get<ir::Document>();
  const bool strip = options::resolved_strip_metadata(ctx.options);
  if (strip) document::strip_metadata(doc);
  const ImagePolicy policy = image_policy(ctx.options);
  // Pictures become image records referenced by recindex.
  std::vector<Bytes> images;
  std::map<std::string, std::size_t> seen;
  int dropped = 0;
  const auto convert = [&](ir::ImageSource& source) {
    auto prepared = prepare_image(source, policy, ctx);
    if (prepared && prepared->extension != "jpg" && prepared->extension != "png" && prepared->extension != "gif") {
      prepared = convert_to_png(*prepared, ctx);
    }
    ir::EncodedImage ref;
    if (!prepared) {
      if (policy.mode == ImageMode::Drop) ++dropped;
      source = ref;
      return;
    }
    const std::string key = util::sha1_hex(prepared->bytes);
    std::size_t index = 0;
    if (auto it = seen.find(key); it != seen.end()) {
      index = it->second;
    } else {
      images.push_back(std::move(prepared->bytes));
      index = images.size();
      seen[key] = index;
    }
    std::string number = std::to_string(index);
    while (number.size() < 5) number.insert(number.begin(), '0');
    ref.url = "recindex:" + number;
    source = std::move(ref);
  };
  std::function<void(ir::Blocks&, int)> walk_blocks;
  std::function<void(ir::Inlines&, int)> walk_inlines = [&](ir::Inlines& inlines, int depth) {
    if (depth > ir::max_document_depth) return;
    for (ir::Inline& item : inlines) {
      if (auto* image = item.get_if<ir::InlineImage>()) convert(image->source);
      std::visit(
          [&](auto& node) {
            if constexpr (requires { node.children; }) walk_inlines(node.children, depth + 1);
          },
          item.node);
    }
  };
  walk_blocks = [&](ir::Blocks& blocks, int depth) {
    if (depth > ir::max_document_depth) return;
    for (ir::Block& b : blocks) {
      if (auto* image = b.get_if<ir::ImageBlock>()) convert(image->source);
      if (auto* p = b.get_if<ir::Paragraph>()) walk_inlines(p->content, 0);
      if (auto* h = b.get_if<ir::Heading>()) walk_inlines(h->content, 0);
      if (auto* l = b.get_if<ir::List>()) {
        for (ir::Blocks& item : l->items) walk_blocks(item, depth + 1);
      }
      if (auto* q = b.get_if<ir::BlockQuote>()) walk_blocks(q->blocks, depth + 1);
      if (auto* t = b.get_if<ir::TableBlock>()) {
        for (ir::TableRow& row : t->rows) {
          for (ir::TableCell& cell : row.cells) walk_inlines(cell.content, 0);
        }
      }
    }
  };
  walk_blocks(doc.blocks, 0);
  for (ir::Footnote& n : doc.footnotes) walk_blocks(n.blocks, 0);
  if (dropped > 0) ctx.warn(std::to_string(dropped) + " picture(s) were left out as requested.");

  document::HtmlWriteOptions options;
  options.standalone = false;
  options.images = document::ImageHandling::Link;
  options.minify = true;
  std::string body = document::write_html(doc, options);
  body = util::replace_all(body, "<div class=\"page-break\" style=\"break-after:page\"></div>", "<mbp:pagebreak/>");
  body = util::replace_all(body, "src=\"recindex:", "recindex=\"");
  std::string html = "<html><head><guide></guide></head><body>" + body + "</body></html>";

  // Text records of exactly 4096 bytes with multibyte overlap bytes.
  constexpr std::size_t record_size = 4096;
  std::vector<Bytes> records;
  records.emplace_back();  // record 0, filled below
  const std::size_t text_length = html.size();
  for (std::size_t at = 0; at < text_length; at += record_size) {
    if (((at / record_size) & 15) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(at) / static_cast<double>(text_length), "Compressing"));
    const std::size_t end = std::min(at + record_size, text_length);
    Bytes record = palm::palmdoc_compress(as_bytes(std::string_view(html).substr(at, end - at)));
    // Continuation bytes of a character that starts in this record.
    std::size_t overlap = 0;
    if (end < text_length) {
      std::size_t start = end;
      while (start > at && (static_cast<unsigned char>(html[start - 1]) & 0xC0) == 0x80) --start;
      if (start > at && static_cast<unsigned char>(html[start - 1]) >= 0xC0) {
        const unsigned char lead = static_cast<unsigned char>(html[start - 1]);
        const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        const std::size_t char_end = start - 1 + length;
        if (char_end > end) overlap = std::min(char_end - end, text_length - end);
      }
    }
    for (std::size_t k = 0; k < overlap; ++k) record.push_back(static_cast<std::byte>(html[end + k]));
    record.push_back(static_cast<std::byte>(overlap));
    records.push_back(std::move(record));
  }
  const std::size_t text_records = records.size() - 1;
  const std::size_t first_image = images.empty() ? null_index : records.size();
  for (Bytes& image : images) records.push_back(std::move(image));
  const std::size_t flis = records.size();
  records.push_back(to_bytes(std::string_view("FLIS\0\0\0\x08\0\x41\0\0\0\0\0\0\xFF\xFF\xFF\xFF\0\x01\0\x03\0\0\0\x03\0\0\0\x01\xFF\xFF\xFF\xFF", 36)));
  const std::size_t fcis = records.size();
  {
    util::ByteWriter w;
    w.text("FCIS").u32be(0x14).u32be(0x10).u32be(1).u32be(0).u32be(static_cast<std::uint32_t>(text_length));
    w.u32be(0).u32be(0x20).u32be(8).u16be(1).u16be(1).u32be(0);
    records.push_back(w.take());
  }
  records.push_back(to_bytes(std::string_view("\xE9\x8E\x0D\x0A", 4)));

  // Record 0: PalmDOC header, MOBI header, EXTH, full name.
  std::string title = doc.metadata.title;
  if (title.empty()) {
    for (const ir::Block& b : doc.blocks) {
      if (const auto* h = b.get_if<ir::Heading>()) {
        title = std::string(util::trim(ir::plain_text(h->content)));
        break;
      }
    }
  }
  if (title.empty()) title = "Untitled";
  std::string language = ctx.options.get_string("language", "");
  if (language.empty()) language = doc.metadata.language;
  struct Exth {
    std::uint32_t type;
    std::string value;
  };
  std::vector<Exth> exth = {{503, title}};
  if (!language.empty()) exth.push_back({524, language});
  if (!strip && !doc.metadata.author.empty()) exth.push_back({100, doc.metadata.author});
  util::ByteWriter ex;
  std::uint32_t exth_length = 12;
  for (const Exth& e : exth) exth_length += 8 + static_cast<std::uint32_t>(e.value.size());
  const std::uint32_t exth_padding = (4 - exth_length % 4) % 4;
  ex.text("EXTH").u32be(exth_length + exth_padding).u32be(static_cast<std::uint32_t>(exth.size()));
  for (const Exth& e : exth) ex.u32be(e.type).u32be(8 + static_cast<std::uint32_t>(e.value.size())).text(e.value);
  ex.zeros(exth_padding);
  const Bytes exth_bytes = ex.take();

  util::ByteWriter r0;
  r0.u16be(2).u16be(0).u32be(static_cast<std::uint32_t>(text_length)).u16be(static_cast<std::uint16_t>(text_records));
  r0.u16be(static_cast<std::uint16_t>(record_size)).u16be(0).u16be(0);
  const std::uint32_t uid = [&] {
    const auto digest = util::sha1(as_bytes(html));
    return (static_cast<std::uint32_t>(digest[0]) << 24) |
           (static_cast<std::uint32_t>(digest[1]) << 16) |
           (static_cast<std::uint32_t>(digest[2]) << 8) |
           static_cast<std::uint32_t>(digest[3]);
  }();
  r0.text("MOBI").u32be(232).u32be(2).u32be(65001).u32be(uid).u32be(6);
  for (int i = 0; i < 10; ++i) r0.u32be(null_index);  // orth, infl, names, keys, extra 0-5
  r0.u32be(static_cast<std::uint32_t>(first_image == null_index ? flis : first_image));  // first non-book record
  const std::uint32_t name_offset = static_cast<std::uint32_t>(16 + 232 + exth_bytes.size());
  r0.u32be(name_offset).u32be(static_cast<std::uint32_t>(title.size()));
  r0.u32be(9).u32be(0).u32be(0).u32be(6);  // locale, input/output language, min version
  r0.u32be(static_cast<std::uint32_t>(first_image));
  r0.u32be(0).u32be(0).u32be(0).u32be(0);  // huffman offset/count, datp offset/count
  r0.u32be(0x50);                           // EXTH present
  r0.zeros(36);
  r0.u32be(null_index).u32be(0).u32be(0).u32be(0);  // DRM offset/count/size/flags
  r0.zeros(8);
  r0.u16be(1).u16be(static_cast<std::uint16_t>(text_records)).u32be(1);
  r0.u32be(static_cast<std::uint32_t>(fcis)).u32be(1).u32be(static_cast<std::uint32_t>(flis)).u32be(1);
  r0.zeros(8);
  r0.u32be(null_index).u32be(0).u32be(null_index).u32be(null_index);
  r0.u32be(1);           // extra record data flags: multibyte overlap
  r0.u32be(null_index);  // primary index
  r0.bytes(exth_bytes);
  r0.text(title);
  r0.zeros(2);
  r0.align(4);
  records[0] = r0.take();
  if (text_records > 65000) return fail(Status::Encode, "the book is too long for the MOBI format");
  return palm::write_database(title, "BOOK", "MOBI", records);
}

int sniff_mobi(ByteView data, std::string_view) {
  if (data.size() < 78) return 0;
  const std::string_view kind = as_chars(data).substr(60, 8);
  if (kind != "BOOKMOBI") return 0;
  // KF8-only books are AZW3.
  return 95;
}

}  // namespace

void register_mobi(Registry& registry) {
  FormatInfo mobi;
  mobi.id = "mobi";
  mobi.name = "Mobipocket e-book";
  mobi.description = "Kindle / Mobipocket e-book (MOBI, PRC, AZW).";
  mobi.category = Category::Ebook;
  mobi.extensions = {"mobi", "prc", "azw"};
  mobi.mime = "application/x-mobipocket-ebook";
  mobi.popularity = 50;
  mobi.signatures = {Signature::at(60, "'BOOKMOBI'")};
  registry.add_format(std::move(mobi));

  FormatInfo azw3;
  azw3.id = "azw3";
  azw3.name = "Kindle KF8 e-book";
  azw3.description = "Kindle Format 8 e-book (AZW3).";
  azw3.category = Category::Ebook;
  azw3.extensions = {"azw3", "kf8"};
  azw3.mime = "application/vnd.amazon.ebook";
  azw3.popularity = 40;
  azw3.sniff = [](ByteView data, std::string_view filename) {
    if (sniff_mobi(data, filename) == 0) return 0;
    // Version 8 in the first header: a KF8-only book.
    return be32(data.subspan(std::min<std::size_t>(data.size(), be32(data, 78))), 36) == 8 ? 97 : 0;
  };
  registry.add_format(std::move(azw3));

  FormatInfo pdb;
  pdb.id = "palmdoc";
  pdb.name = "Palm e-book";
  pdb.description = "PalmDOC e-book (Palm database).";
  pdb.category = Category::Ebook;
  pdb.extensions = {"pdb"};
  pdb.mime = "application/vnd.palm";
  pdb.popularity = 10;
  pdb.signatures = {Signature::at(60, "'TEXtREAd'")};
  registry.add_format(std::move(pdb));

  for (const char* id : {"mobi", "azw3", "palmdoc"}) {
    Codec reader;
    reader.format_id = id;
    reader.kind = Kind::Document;
    reader.read = read_mobi;
    reader.read_options = epub_read_options();
    registry.add_codec(std::move(reader));
  }
  Codec writer;
  writer.format_id = "mobi";
  writer.kind = Kind::Document;
  writer.write = write_mobi;
  std::vector<OptionSpec> write_options = {
      OptionSpec::text("language", "Language", "", "Language of the book as a BCP 47 tag (empty: from the source).")
          .advanced()};
  for (OptionSpec& spec : image_write_options()) write_options.push_back(std::move(spec));
  write_options.push_back(options::strip_metadata());
  writer.write_options = std::move(write_options);
  registry.add_codec(std::move(writer));
}

}  // namespace cvt::office
