// Convertible core - office family: Outlook messages (MSG) and TNEF
// (winmail.dat).
//
// MSG: the property streams of the compound file - subject, sender,
// recipients, dates, plain/HTML/compressed-RTF bodies and attachments
// (with content ids so inline pictures of HTML bodies resolve). TNEF: the
// attribute stream with its embedded MAPI property lists. Both read as a
// Document (headers + body + attachment list) and as an Archive of the
// attachments. Compressed RTF ("LZFu") is decompressed in-house.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "families/table/cfb.hpp"
#include "families/table/codepages.hpp"
#include "ir/archive.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

std::uint32_t le32(ByteView d, std::size_t at) {
  util::ByteReader r(d);
  if (!r.seek(at)) return 0;
  return r.u32le();
}

// --------------------------------------------------------------------------- compressed RTF

constexpr std::string_view rtf_prebuffer =
    "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans "
    "SerifSymbolArialTimes New RomanCourier{\\colortbl\\red0\\green0\\blue0\r\n\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx";

Result<std::string> decompress_rtf(ByteView data, std::uint64_t max_out) {
  if (data.size() < 16) return fail(Status::Decode, "the compressed RTF body is truncated");
  const std::uint32_t raw_size = le32(data, 4);
  const std::uint32_t type = le32(data, 8);
  if (type == 0x414C454Du) {  // "MELA": stored
    return std::string(as_chars(data.subspan(16, std::min<std::size_t>(raw_size, data.size() - 16))));
  }
  if (type != 0x75465A4Cu) return fail(Status::Decode, "unknown compressed RTF type");
  if (raw_size > max_out) return fail(Status::Limit, "the RTF body exceeds the limit");
  std::array<char, 4096> dict{};
  std::copy(rtf_prebuffer.begin(), rtf_prebuffer.end(), dict.begin());
  std::size_t write = rtf_prebuffer.size();
  std::string out;
  out.reserve(raw_size);
  std::size_t at = 16;
  const auto bytes = as_u8(data);
  while (at < bytes.size() && out.size() < raw_size) {
    const std::uint8_t flags = bytes[at++];
    for (int bit = 0; bit < 8 && at < bytes.size() && out.size() < raw_size; ++bit) {
      if (flags & (1 << bit)) {
        if (at + 1 >= bytes.size()) return out;
        const std::uint32_t ref = (static_cast<std::uint32_t>(bytes[at]) << 8) | bytes[at + 1];
        at += 2;
        const std::size_t offset = ref >> 4;
        const std::size_t length = (ref & 0x0F) + 2;
        if (offset == write) return out;  // end marker
        for (std::size_t k = 0; k < length; ++k) {
          const char c = dict[(offset + k) % 4096];
          out.push_back(c);
          dict[write] = c;
          write = (write + 1) % 4096;
        }
      } else {
        const char c = static_cast<char>(bytes[at++]);
        out.push_back(c);
        dict[write] = c;
        write = (write + 1) % 4096;
      }
    }
  }
  return out;
}

// --------------------------------------------------------------------------- common message model

struct Attachment {
  std::string name;
  std::string mime;
  std::string content_id;
  Bytes data;
};

struct Message {
  std::string subject, from, to, cc, date;
  std::string body;      // plain text
  Bytes html;            // HTML body (bytes, any charset)
  std::string rtf;       // decompressed RTF body
  std::vector<Attachment> attachments;
};

std::string filetime_to_iso(std::uint64_t ft) {
  if (ft == 0) return {};
  const std::int64_t seconds = static_cast<std::int64_t>(ft / 10000000ULL) - 11644473600LL;
  if (seconds < 0) return {};
  std::int64_t days = seconds / 86400;
  std::int64_t rem = seconds % 86400;
  // Civil from days (Howard Hinnant).
  days += 719468;
  const std::int64_t era = days / 146097;
  const std::int64_t doe = days - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp + (mp < 10 ? 3 : -9);
  if (m <= 2) ++y;
  const auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
  return std::to_string(y) + "-" + two(m) + "-" + two(d) + "T" + two(rem / 3600) + ":" + two(rem % 3600 / 60) + ":" +
         two(rem % 60) + "Z";
}

Result<ir::Document> message_document(Message& m, const ReadContext& ctx) {
  ir::Document doc;
  ir::Document body;
  if (!m.html.empty()) {
    document::HtmlReadOptions options;
    options.read_metadata = false;
    std::map<std::string, const Attachment*> by_cid;
    for (const Attachment& a : m.attachments) {
      if (!a.content_id.empty()) by_cid["cid:" + a.content_id] = &a;
    }
    options.resolve = [&by_cid](std::string_view url) -> std::optional<ir::EncodedImage> {
      const auto it = by_cid.find(std::string(url));
      if (it == by_cid.end()) return std::nullopt;
      ir::EncodedImage image;
      image.bytes = it->second->data;
      image.mime = document::sniff_image_mime(image.bytes);
      return image;
    };
    CVT_TRY_ASSIGN(body, document::parse_html(m.html, options));
  } else if (!m.rtf.empty()) {
    auto parsed = rtf_to_document(m.rtf, ctx);
    if (parsed) body = std::move(*parsed);
  }
  if (body.blocks.empty() && !m.body.empty()) body = document::infer_document_from_text(m.body);
  doc.metadata.title = m.subject;
  doc.blocks.push_back(ir::Heading{1, {ir::Text{m.subject.empty() ? std::string("(no subject)") : m.subject}}});
  ir::Inlines header;
  const auto line = [&](std::string_view label, const std::string& value) {
    if (value.empty()) return;
    if (!header.empty()) header.push_back(ir::LineBreak{});
    header.push_back(ir::Strong{{ir::Text{std::string(label)}}});
    header.push_back(ir::Text{" " + value});
  };
  line("From:", m.from);
  line("To:", m.to);
  line("Cc:", m.cc);
  line("Date:", m.date);
  if (!header.empty()) doc.blocks.push_back(ir::Paragraph{std::move(header)});
  doc.blocks.push_back(ir::HorizontalRule{});
  for (ir::Block& b : body.blocks) doc.blocks.push_back(std::move(b));
  for (ir::Footnote& n : body.footnotes) doc.footnotes.push_back(std::move(n));
  std::vector<std::string> names;
  for (const Attachment& a : m.attachments) {
    if (!a.name.empty()) names.push_back(a.name);
  }
  if (!names.empty()) {
    doc.blocks.push_back(ir::HorizontalRule{});
    doc.blocks.push_back(ir::Paragraph{{ir::Strong{{ir::Text{"Attachments:"}}}, ir::Text{" " + util::join(names, ", ")}}});
  }
  return doc;
}

ir::Archive message_archive(Message& m) {
  ir::Archive archive;
  std::map<std::string, int> used;
  int unnamed = 0;
  for (Attachment& a : m.attachments) {
    std::string name = a.name.empty() ? "attachment-" + std::to_string(++unnamed) : a.name;
    auto safe = ir::sanitize_archive_path(util::sanitize_utf8(name));
    std::string path = safe ? *safe : "attachment-" + std::to_string(++unnamed);
    if (const int n = used[path]++; n > 0) {
      const std::size_t dot = path.rfind('.');
      path = dot == std::string::npos ? path + "-" + std::to_string(n) : path.substr(0, dot) + "-" + std::to_string(n) + path.substr(dot);
    }
    ir::ArchiveEntry entry;
    entry.path = std::move(path);
    entry.bytes = std::move(a.data);
    archive.entries.push_back(std::move(entry));
  }
  return archive;
}

// --------------------------------------------------------------------------- MSG

std::optional<std::string> msg_string(const table::CfbReader& cfb, std::string_view prefix, std::uint16_t tag) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string hex;
  for (int shift = 12; shift >= 0; shift -= 4) hex.push_back(digits[(tag >> shift) & 15]);
  const std::string base = std::string(prefix) + "__substg1.0_" + hex;
  if (auto u = cfb.read(base + "001F")) {
    std::u16string text;
    for (std::size_t k = 0; k + 1 < u->size(); k += 2) {
      text.push_back(static_cast<char16_t>(std::to_integer<std::uint16_t>((*u)[k]) | (std::to_integer<std::uint16_t>((*u)[k + 1]) << 8)));
    }
    while (!text.empty() && text.back() == 0) text.pop_back();
    return util::utf16_to_utf8(text);
  }
  if (auto a = cfb.read(base + "001E")) {
    std::string text(as_chars(*a));
    text = text.substr(0, text.find('\0'));
    return table::decode_code_page(as_bytes(text), *table::find_code_page(1252));
  }
  return std::nullopt;
}

std::optional<Bytes> msg_binary(const table::CfbReader& cfb, std::string_view prefix, std::uint16_t tag) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string hex;
  for (int shift = 12; shift >= 0; shift -= 4) hex.push_back(digits[(tag >> shift) & 15]);
  auto b = cfb.read(std::string(prefix) + "__substg1.0_" + hex + "0102");
  if (!b) return std::nullopt;
  return std::move(*b);
}

Result<Message> read_msg_message(const ReadContext& ctx) {
  CVT_TRY_ASSIGN(const table::CfbReader cfb, table::CfbReader::open(ctx.input, ctx.limits));
  if (!cfb.contains("__properties_version1.0")) return fail(Status::Decode, "the compound file is not an Outlook message");
  Message m;
  m.subject = msg_string(cfb, "", 0x0037).value_or("");
  m.from = msg_string(cfb, "", 0x0C1A).value_or("");
  const std::string address = msg_string(cfb, "", 0x5D01).value_or(msg_string(cfb, "", 0x0C1F).value_or(""));
  if (!address.empty() && address.find('@') != std::string::npos) m.from += (m.from.empty() ? "" : " ") + std::string("<") + address + ">";
  m.to = msg_string(cfb, "", 0x0E04).value_or("");
  m.cc = msg_string(cfb, "", 0x0E03).value_or("");
  m.body = msg_string(cfb, "", 0x1000).value_or("");
  if (auto html = msg_binary(cfb, "", 0x1013)) {
    m.html = std::move(*html);
  } else if (auto html_text = msg_string(cfb, "", 0x1013)) {
    m.html = to_bytes(*html_text);
  }
  if (auto rtf = msg_binary(cfb, "", 0x1009)) {
    auto text = decompress_rtf(*rtf, ctx.limits.max_decompressed_bytes);
    if (text) m.rtf = std::move(*text);
  }
  // Dates from the fixed-size property stream (header of 32 bytes at the top level).
  if (auto props = cfb.read("__properties_version1.0")) {
    std::uint64_t sent = 0;
    for (std::size_t at = 32; at + 16 <= props->size(); at += 16) {
      const std::uint32_t tag = le32(*props, at);
      if ((tag & 0xFFFF) != 0x0040) continue;
      const std::uint64_t value = le32(*props, at + 8) | (std::uint64_t{le32(*props, at + 12)} << 32);
      if ((tag >> 16) == 0x0039 || ((tag >> 16) == 0x0E06 && sent == 0)) sent = value;
    }
    m.date = filetime_to_iso(sent);
  }
  // Attachments.
  for (const table::CfbEntry& e : cfb.entries()) {
    if (e.type != table::CfbEntryType::Storage || !e.name.starts_with("__attach_version1.0_")) continue;
    if (e.parent && *e.parent != 0) continue;  // attachments of embedded messages
    const std::string prefix = e.path + "/";
    Attachment a;
    a.name = msg_string(cfb, prefix, 0x3707).value_or(msg_string(cfb, prefix, 0x3704).value_or(""));
    a.mime = msg_string(cfb, prefix, 0x370E).value_or("");
    a.content_id = msg_string(cfb, prefix, 0x3712).value_or("");
    if (auto data = msg_binary(cfb, prefix, 0x3701)) a.data = std::move(*data);
    if (a.data.empty() && a.name.empty()) continue;
    m.attachments.push_back(std::move(a));
  }
  return m;
}

// --------------------------------------------------------------------------- TNEF

void read_mapi(ByteView data, Message& m, Attachment* attachment) {
  util::ByteReader r(data);
  const std::uint32_t count = r.u32le();
  for (std::uint32_t i = 0; i < count && r.ok() && i < 10000; ++i) {
    const std::uint16_t type = r.u16le();
    const std::uint16_t id = r.u16le();
    if (id >= 0x8000) {  // named property: GUID, kind, id or name
      r.skip(16);
      const std::uint32_t kind = r.u32le();
      if (kind == 0) {
        r.skip(4);
      } else {
        const std::uint32_t length = r.u32le();
        r.skip((static_cast<std::size_t>(length) + 3) / 4 * 4);
      }
    }
    const bool multi = (type & 0x1000) != 0;
    const std::uint16_t base = type & 0x0FFF;
    std::uint32_t values = multi ? r.u32le() : 1;
    const bool variable = base == 0x001E || base == 0x001F || base == 0x0102 || base == 0x000D;
    if (variable && !multi) values = r.u32le();
    for (std::uint32_t v = 0; v < values && r.ok() && v < 100000; ++v) {
      if (variable) {
        const std::uint32_t length = r.u32le();
        const ByteView value = r.bytes(length);
        r.skip((4 - length % 4) % 4);
        if (!r.ok()) return;
        std::string text;
        if (base == 0x001F) {
          std::u16string u;
          for (std::size_t k = 0; k + 1 < value.size(); k += 2) {
            u.push_back(static_cast<char16_t>(std::to_integer<std::uint16_t>(value[k]) | (std::to_integer<std::uint16_t>(value[k + 1]) << 8)));
          }
          while (!u.empty() && u.back() == 0) u.pop_back();
          text = util::utf16_to_utf8(u);
        } else if (base == 0x001E) {
          text = std::string(as_chars(value));
          text = table::decode_code_page(as_bytes(text.substr(0, text.find('\0'))), *table::find_code_page(1252));
        }
        if (attachment) {
          if (id == 0x3707 && !text.empty()) attachment->name = text;
          if (id == 0x370E) attachment->mime = text;
          if (id == 0x3712) attachment->content_id = text;
          if (id == 0x3701 && base == 0x0102) attachment->data = to_bytes(value);
        } else {
          if (id == 0x1000 && !text.empty()) m.body = text;
          if (id == 0x0037 && !text.empty()) m.subject = text;
          if (id == 0x1013) m.html = base == 0x0102 ? to_bytes(value) : to_bytes(text);
          if (id == 0x1009 && base == 0x0102) {
            if (auto rtf = decompress_rtf(value, 64 * 1024 * 1024)) m.rtf = std::move(*rtf);
          }
        }
      } else {
        std::size_t size = 4;
        if (base == 0x0014 || base == 0x0040 || base == 0x0005 || base == 0x0006 || base == 0x0007) size = 8;
        if (base == 0x0048) size = 16;
        r.skip(size);
      }
    }
  }
}

Result<Message> read_tnef_message(const ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u32le() != 0x223E9F78u) return fail(Status::Decode, "not a TNEF stream");
  r.skip(2);  // key
  Message m;
  Attachment* current = nullptr;
  std::uint64_t total = 0;
  while (r.remaining() >= 9) {
    const std::uint8_t level = r.u8();
    const std::uint32_t id = r.u32le();
    const std::uint32_t length = r.u32le();
    const ByteView data = r.bytes(length);
    r.skip(2);  // checksum
    if (!r.ok()) return fail(Status::Decode, "the TNEF stream is truncated");
    total += length;
    CVT_TRY(ctx.limits.check_decompressed(total));
    const std::uint16_t tag = id & 0xFFFF;
    if (level == 1) {
      if (tag == 0x8004) m.subject = std::string(as_chars(data).substr(0, as_chars(data).find('\0')));
      if (tag == 0x800C) m.body = std::string(as_chars(data).substr(0, as_chars(data).find('\0')));
      if (tag == 0x9003) read_mapi(data, m, nullptr);
    } else if (level == 2) {
      if (tag == 0x9002) {  // attAttachRendData starts an attachment
        CVT_TRY(ctx.limits.check_archive_entries(m.attachments.size() + 1));
        m.attachments.emplace_back();
        current = &m.attachments.back();
      } else if (current) {
        if (tag == 0x8010) current->name = std::string(as_chars(data).substr(0, as_chars(data).find('\0')));
        if (tag == 0x800F) current->data = to_bytes(data);
        if (tag == 0x9005) read_mapi(data, m, current);
      }
    }
  }
  m.subject = table::decode_code_page(as_bytes(m.subject), *table::find_code_page(1252));
  return m;
}

Result<AnyIR> read_msg(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Message m, read_msg_message(ctx));
  CVT_TRY_ASSIGN(ir::Document doc, message_document(m, ctx));
  return AnyIR(std::move(doc));
}
Result<AnyIR> read_msg_archive(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Message m, read_msg_message(ctx));
  return AnyIR(message_archive(m));
}
Result<AnyIR> read_tnef(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Message m, read_tnef_message(ctx));
  CVT_TRY_ASSIGN(ir::Document doc, message_document(m, ctx));
  return AnyIR(std::move(doc));
}
Result<AnyIR> read_tnef_archive(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Message m, read_tnef_message(ctx));
  return AnyIR(message_archive(m));
}

}  // namespace

Result<std::string> decompress_compressed_rtf(ByteView data, std::uint64_t max_out) { return decompress_rtf(data, max_out); }

void register_mail(Registry& registry) {
  FormatInfo msg;
  msg.id = "msg";
  msg.name = "Outlook message";
  msg.description = "Microsoft Outlook e-mail message (compound file).";
  msg.category = Category::Document;
  msg.extensions = {"msg"};
  msg.mime = "application/vnd.ms-outlook";
  msg.popularity = 30;
  msg.sniff = [](ByteView data, std::string_view) { return compound_file_has(data, u"__properties_version1.0") ? 96 : 0; };
  registry.add_format(std::move(msg));
  FormatInfo tnef;
  tnef.id = "tnef";
  tnef.name = "TNEF attachment (winmail.dat)";
  tnef.description = "Transport Neutral Encapsulation Format, the winmail.dat attachment of Outlook mail.";
  tnef.category = Category::Document;
  tnef.extensions = {"dat", "tnef"};
  tnef.mime = "application/ms-tnef";
  tnef.popularity = 10;
  tnef.signatures = {Signature::at(0, "78 9F 3E 22")};
  registry.add_format(std::move(tnef));
  const auto add = [&](const char* id, ReadFn doc_reader, ReadFn archive_reader) {
    Codec d;
    d.format_id = id;
    d.kind = Kind::Document;
    d.read = std::move(doc_reader);
    registry.add_codec(std::move(d));
    Codec a;
    a.format_id = id;
    a.kind = Kind::Archive;
    a.read = std::move(archive_reader);
    a.read_cost = 20;
    registry.add_codec(std::move(a));
  };
  add("msg", read_msg, read_msg_archive);
  add("tnef", read_tnef, read_tnef_archive);
}

}  // namespace cvt::office
