// Convertible core - office family: smaller document and e-book formats.
//
//  * FictionBook in ZIP (.fb2.zip, .fbz): unwrapped and read with the
//    document family's FB2 reader through the registry; written the same way.
//  * TCR (Psion text compression) e-books: read.
//  * AbiWord (.abw, gzip-compressed .zabw): read - paragraphs with heading
//    styles, character properties, links, lists, tables, pictures, notes.
//  * Hangul HWPX (OWPML, ZIP + XML): read - paragraphs, runs, tables.
//  * Hangul HWP 5 (compound file, deflated record streams): read - text of
//    the body sections, paragraph by paragraph.
//  * Visio VSDX: read - the text of every shape, page by page.
//  * CHM and Microsoft LIT: recognised; reading needs the LZX decompressor of
//    the archive family and fails with a clear message while it is absent.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
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
#include "util/base64.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::office {

namespace {

using util::XmlNode;

const Codec* find_codec(const Context& ctx, std::string_view format, Kind kind, bool reader) {
  const Registry& registry = ctx.registry ? *ctx.registry : Registry::instance();
  for (const Codec* codec : registry.codecs_for(format)) {
    if (codec->kind == kind && (reader ? static_cast<bool>(codec->read) : static_cast<bool>(codec->write))) return codec;
  }
  return nullptr;
}

// --------------------------------------------------------------------------- FB2 in ZIP

Result<AnyIR> read_fbz(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
  std::optional<std::size_t> member;
  for (std::size_t i = 0; i < zip.size(); ++i) {
    if (util::iends_with(zip.entries()[i].name, ".fb2")) {
      member = i;
      break;
    }
  }
  if (!member) return fail(Status::Decode, "the archive contains no FictionBook file");
  CVT_TRY_ASSIGN(const Bytes fb2, zip.read(*member));
  const Codec* reader = find_codec(ctx, "fb2", Kind::Document, true);
  if (!reader) return fail(Status::UnsupportedConversion, "the FictionBook reader is not available in this build");
  ReadContext inner;
  inner.options = ctx.options;
  inner.limits = ctx.limits;
  inner.progress = ctx.progress;
  inner.warnings = ctx.warnings;
  inner.registry = ctx.registry;
  inner.input = fb2;
  inner.format_id = "fb2";
  inner.filename = zip.entries()[*member].name;
  CVT_TRY_ASSIGN(AnyIR value, reader->read(inner));
  // The FB2 bytes die here: copy everything the document refers to (it holds no views).
  return value;
}

Result<Bytes> write_fbz(const AnyIR& value, WriteContext& ctx) {
  const Codec* writer = find_codec(ctx, "fb2", Kind::Document, false);
  if (!writer) return fail(Status::UnsupportedConversion, "the FictionBook writer is not available in this build");
  CVT_TRY_ASSIGN(const Bytes fb2, writer->write(value, ctx));
  util::ZipWriter zip;
  util::ZipEntryOptions entry;
  entry.level = 9;
  std::string name = value.get<ir::Document>().metadata.title;
  std::string safe;
  for (char c : name) safe.push_back(util::is_ascii_alpha(c) || util::is_ascii_digit(c) ? c : '_');
  if (safe.empty() || safe.size() > 64) safe = "book";
  CVT_TRY(zip.add(safe + ".fb2", fb2, entry));
  return zip.finish();
}

// --------------------------------------------------------------------------- TCR

Result<AnyIR> read_tcr(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("!!8-Bit!!")) return fail(Status::Decode, "not a TCR file");
  std::vector<std::string> dict(256);
  for (std::string& entry : dict) {
    const std::uint8_t length = r.u8();
    entry = std::string(r.chars(length));
  }
  if (!r.ok()) return fail(Status::Decode, "the TCR dictionary is truncated");
  std::string text;
  const std::uint64_t cap = std::min<std::uint64_t>(ctx.limits.max_decompressed_bytes, 256ull * 1024 * 1024);
  while (!r.at_end()) {
    text += dict[r.u8()];
    if (text.size() > cap) return fail(Status::Limit, "the TCR text exceeds the limit");
  }
  const util::EncodingGuess guess = util::guess_encoding(as_bytes(text));
  std::string utf8 = util::decode_text(as_bytes(text), guess.encoding).text;
  ir::Document doc = document::infer_document_from_text(util::convert_line_endings(utf8, util::LineEnding::Lf));
  return AnyIR(std::move(doc));
}

// --------------------------------------------------------------------------- AbiWord

class AbwReader {
 public:
  AbwReader(const ReadContext& ctx, ReadSettings s) : ctx_(ctx), settings_(s) {}
  Result<ir::Document> read(const util::XmlDocument& xml);

 private:
  void blocks(XmlNode node, FlowBuilder& flow, int depth);
  void spans(XmlNode node, InlineBuilder& out, RunFormat f, int depth);
  RunFormat props(std::string_view css, RunFormat f) const;

  const ReadContext& ctx_;
  ReadSettings settings_;
  std::map<std::string, ir::EncodedImage> data_;
  std::map<std::string, bool> list_ordered_;
  std::vector<ir::Footnote> notes_;
};

RunFormat AbwReader::props(std::string_view css, RunFormat f) const {
  for (std::string_view decl : util::split(css, ';', false)) {
    const std::size_t colon = decl.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string_view key = util::trim(decl.substr(0, colon));
    const std::string_view value = util::trim(decl.substr(colon + 1));
    if (key == "font-weight") f.bold = value == "bold";
    if (key == "font-style") f.italic = value == "italic";
    if (key == "text-decoration") {
      f.underline = value.find("underline") != std::string_view::npos;
      f.strike = value.find("line-through") != std::string_view::npos;
    }
    if (key == "text-position") {
      f.superscript = value == "superscript";
      f.subscript = value == "subscript";
    }
    if (key == "font-family") {
      const std::string lower = util::to_lower(value);
      f.code = lower.find("mono") != std::string::npos || lower.find("courier") != std::string::npos;
    }
  }
  return f;
}

void AbwReader::spans(XmlNode node, InlineBuilder& out, RunFormat f, int depth) {
  if (depth > 32) return;
  for (XmlNode c = node.first_child(); c; c = c.next_sibling()) {
    if (c.is_text()) {
      out.text(c.value(), f);
      continue;
    }
    if (!c.is_element()) continue;
    const std::string_view name = c.local_name();
    if (name == "c") {
      spans(c, out, props(attr(c, "props"), f), depth + 1);
    } else if (name == "a") {
      RunFormat g = f;
      g.link = std::string(attr(c, "href"));
      spans(c, out, g, depth + 1);
    } else if (name == "br") {
      out.line_break(f);
    } else if (name == "image") {
      if (auto it = data_.find(std::string(attr(c, "dataid"))); it != data_.end()) {
        out.node(ir::InlineImage{it->second, std::string(attr(c, "alt"))}, RunFormat{});
      }
    } else if (name == "foot" || name == "endnote") {
      if (!settings_.notes) continue;
      FlowBuilder flow;
      blocks(c, flow, depth + 1);
      const std::string label = std::to_string(notes_.size() + 1);
      notes_.push_back(ir::Footnote{label, flow.take()});
      ir::Link link;
      link.url = "#fn-" + label;
      link.note = label;
      link.children.push_back(ir::Text{label});
      out.node(std::move(link), RunFormat{});
    } else if (name == "field" || name == "bookmark") {
      continue;
    }
  }
}

void AbwReader::blocks(XmlNode node, FlowBuilder& flow, int depth) {
  if (depth > 24) return;
  for (XmlNode c : node.elements()) {
    const std::string_view name = c.local_name();
    if (name == "section") {
      blocks(c, flow, depth + 1);
    } else if (name == "p") {
      InlineBuilder out;
      spans(c, out, props(attr(c, "props"), RunFormat{}), depth + 1);
      ir::Inlines content = out.take();
      trim_inlines(content);
      if (content.empty()) continue;
      const std::string style(attr(c, "style"));
      const int heading = heading_level_from_style_name(style);
      const std::string list(attr(c, "listid"));
      if (heading > 0) {
        flow.block(ir::Heading{heading, std::move(content)});
      } else if (!list.empty() && list != "0") {
        FlowBuilder::ListInfo info;
        info.list_id = list;
        info.level = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(c, "level")).value_or(1) - 1, 0, 8));
        const std::string list_props(attr(c, "props"));
        info.ordered = list_props.find("list-style:Numbered") != std::string::npos ||
                       list_props.find("list-style:Upper") != std::string::npos ||
                       list_props.find("list-style:Lower") != std::string::npos ||
                       (list_props.find("list-style:") == std::string::npos && list_ordered_[list]);
        flow.list_item(info, ir::Paragraph{std::move(content)});
      } else if (util::to_lower(style).find("plain text") != std::string::npos) {
        flow.block(ir::CodeBlock{{}, ir::plain_text(content)});
      } else if (util::to_lower(style).find("block text") != std::string::npos) {
        flow.block(ir::BlockQuote{{ir::Paragraph{std::move(content)}}});
      } else {
        flow.block(ir::Paragraph{std::move(content)});
      }
    } else if (name == "table") {
      ir::TableBlock table;
      std::map<std::pair<int, int>, ir::TableCell> cells;
      int rows = 0;
      for (XmlNode cell : c.children_local("cell")) {
        int top = 0, left = 0, bottom = 1, right = 1;
        for (std::string_view decl : util::split(attr(cell, "props"), ';', false)) {
          const std::size_t colon = decl.find(':');
          if (colon == std::string_view::npos) continue;
          const std::string_view key = util::trim(decl.substr(0, colon));
          const int v = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(util::trim(decl.substr(colon + 1))).value_or(0), 0, 1000));
          if (key == "top-attach") top = v;
          if (key == "bot-attach") bottom = v;
          if (key == "left-attach") left = v;
          if (key == "right-attach") right = v;
        }
        FlowBuilder inner;
        blocks(cell, inner, depth + 1);
        ir::TableCell tc;
        tc.colspan = std::max(1, right - left);
        tc.rowspan = std::max(1, bottom - top);
        for (const ir::Block& b : inner.take()) {
          std::string text = ir::plain_text(ir::Blocks{b});
          while (!text.empty() && text.back() == '\n') text.pop_back();
          if (const auto* p = b.get_if<ir::Paragraph>()) {
            if (!tc.content.empty()) tc.content.push_back(ir::LineBreak{});
            for (const ir::Inline& x : p->content) tc.content.push_back(x);
          } else if (!text.empty()) {
            if (!tc.content.empty()) tc.content.push_back(ir::LineBreak{});
            for (ir::Inline& x : text_with_breaks(text)) tc.content.push_back(std::move(x));
          }
        }
        cells[{top, left}] = std::move(tc);
        rows = std::max(rows, bottom);
        if (cells.size() > 100000) break;
      }
      for (int r = 0; r < rows && r < 10000; ++r) {
        ir::TableRow row;
        for (auto it = cells.lower_bound({r, 0}); it != cells.end() && it->first.first == r; ++it) row.cells.push_back(std::move(it->second));
        table.rows.push_back(std::move(row));
      }
      flow.block(std::move(table));
    }
  }
}

Result<ir::Document> AbwReader::read(const util::XmlDocument& xml) {
  const XmlNode root = xml.root();
  if (root.local_name() != "abiword") return fail(Status::Decode, "not an AbiWord document");
  for (XmlNode d : child(root, "data").children_local("d")) {
    std::string text;
    for (char c : d.text()) {
      if (!util::is_ascii_space(c)) text.push_back(c);
    }
    ir::EncodedImage image;
    if (attr(d, "base64") == "yes") {
      auto decoded = util::base64_decode(text);
      if (!decoded) continue;
      image.bytes = std::move(*decoded);
    } else {
      image.bytes = to_bytes(d.text());
    }
    image.mime = std::string(attr(d, "mime-type"));
    data_[std::string(attr(d, "name"))] = std::move(image);
  }
  for (XmlNode l : child(root, "lists").children_local("l")) {
    const std::string_view type = attr(l, "list-style");
    list_ordered_[std::string(attr(l, "id"))] = type.find("Bullet") == std::string_view::npos && type.find("Dash") == std::string_view::npos &&
                                                 type.find("Square") == std::string_view::npos;
  }
  ir::Document doc;
  for (XmlNode m : child(root, "metadata").children_local("m")) {
    const std::string_view key = attr(m, "key");
    if (key == "dc.title") doc.metadata.title = std::string(util::trim(m.text()));
    if (key == "dc.creator") doc.metadata.author = std::string(util::trim(m.text()));
    if (key == "dc.language") doc.metadata.language = std::string(util::trim(m.text()));
  }
  FlowBuilder flow;
  blocks(root, flow, 0);
  doc.blocks = flow.take();
  doc.footnotes = std::move(notes_);
  (void)ctx_;
  finish_metadata(doc, settings_);
  return doc;
}

Result<AnyIR> read_abw(ReadContext& ctx) {
  Bytes inflated;
  ByteView data = ctx.input;
  if (as_chars(data).starts_with("\x1F\x8B")) {
    CVT_TRY_ASSIGN(inflated, util::gzip_decompress(data, ctx.limits.max_decompressed_bytes));
    data = inflated;
  }
  CVT_TRY_ASSIGN(util::XmlDocument xml, parse_untrusted_xml(data, true));
  AbwReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read(xml));
  return AnyIR(std::move(doc));
}

// --------------------------------------------------------------------------- HWPX

void hwpx_paragraphs(XmlNode node, FlowBuilder& flow, int depth) {
  if (depth > 24) return;
  for (XmlNode c : node.elements()) {
    if (c.local_name() == "p") {
      InlineBuilder out;
      for (XmlNode run : c.children_local("run")) {
        for (XmlNode t : run.elements()) {
          if (t.local_name() == "t") out.text(t.text(), RunFormat{});
          if (t.local_name() == "tbl") {
            ir::TableBlock table;
            for (XmlNode tr : t.children_local("tr")) {
              ir::TableRow row;
              for (XmlNode tc : tr.children_local("tc")) {
                ir::TableCell cell;
                FlowBuilder inner;
                hwpx_paragraphs(child(tc, "subList"), inner, depth + 1);
                std::string text = ir::plain_text(inner.take());
                while (!text.empty() && text.back() == '\n') text.pop_back();
                cell.content = text_with_breaks(text);
                if (XmlNode span = child(tc, "cellSpan")) {
                  cell.colspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(span, "colSpan")).value_or(1), 1, 1000));
                  cell.rowspan = static_cast<int>(std::clamp<std::int64_t>(util::parse_int(attr(span, "rowSpan")).value_or(1), 1, 1000));
                }
                row.cells.push_back(std::move(cell));
              }
              table.rows.push_back(std::move(row));
            }
            flow.block(std::move(table));
          }
        }
      }
      ir::Inlines content = out.take();
      trim_inlines(content);
      if (!content.empty()) flow.block(ir::Paragraph{std::move(content)});
    }
  }
}

Result<AnyIR> read_hwpx(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Package package, Package::open(ctx.input, ctx.limits));
  std::vector<std::string> sections;
  for (const std::string& name : package.names()) {
    if (util::istarts_with(name, "Contents/section") && util::iends_with(name, ".xml")) sections.push_back(name);
  }
  std::ranges::sort(sections, [](const std::string& a, const std::string& b) {
    return a.size() != b.size() ? a.size() < b.size() : a < b;
  });
  if (sections.empty()) return fail(Status::Decode, "the HWPX package has no sections");
  ir::Document doc;
  for (const std::string& name : sections) {
    CVT_TRY_ASSIGN(util::XmlDocument xml, package.xml(name, true));
    FlowBuilder flow;
    hwpx_paragraphs(xml.root(), flow, 0);
    for (ir::Block& b : flow.take()) doc.blocks.push_back(std::move(b));
  }
  return AnyIR(std::move(doc));
}

// --------------------------------------------------------------------------- HWP 5

Result<AnyIR> read_hwp(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const table::CfbReader cfb, table::CfbReader::open(ctx.input, ctx.limits));
  CVT_TRY_ASSIGN(const Bytes header, cfb.read("FileHeader"));
  if (header.size() < 40 || !as_chars(header).starts_with("HWP Document File")) return fail(Status::Decode, "not an HWP 5 document");
  const std::uint32_t flags = std::to_integer<std::uint32_t>(header[36]) | (std::to_integer<std::uint32_t>(header[37]) << 8);
  if (flags & 2) return fail(Status::Decode, "the HWP document is password-protected; it cannot be read");
  if (flags & 4) return fail(Status::Decode, "HWP distribution documents are encrypted; they cannot be read");
  const bool compressed = flags & 1;
  ir::Document doc;
  for (int section = 0; section < 1024; ++section) {
    auto raw = cfb.read("BodyText/Section" + std::to_string(section));
    if (!raw) break;
    Bytes data = std::move(*raw);
    if (compressed) {
      CVT_TRY_ASSIGN(data, util::inflate_raw(data, ctx.limits.max_decompressed_bytes));
    }
    util::ByteReader r(data);
    while (r.remaining() >= 4) {
      const std::uint32_t head = r.u32le();
      const std::uint32_t tag = head & 0x3FF;
      std::uint32_t size = head >> 20;
      if (size == 0xFFF) size = r.u32le();
      const ByteView body = r.bytes(size);
      if (!r.ok()) break;
      if (tag != 67) continue;  // HWPTAG_PARA_TEXT
      std::u16string text;
      for (std::size_t k = 0; k + 1 < body.size(); k += 2) {
        const char16_t c = static_cast<char16_t>(std::to_integer<std::uint16_t>(body[k]) | (std::to_integer<std::uint16_t>(body[k + 1]) << 8));
        if (c < 32) {
          // Inline and extended controls occupy eight code units.
          const bool wide = (c >= 1 && c <= 9) || (c >= 11 && c <= 12) || (c >= 14 && c <= 23);
          if (c == 10) text.push_back(u'\n');
          if (c == 9) text.push_back(u'\t');
          if (wide) k += 14;
          continue;
        }
        text.push_back(c);
      }
      std::string utf8(util::trim(util::utf16_to_utf8(text)));
      if (!utf8.empty()) doc.blocks.push_back(ir::Paragraph{text_with_breaks(utf8)});
    }
  }
  if (doc.blocks.empty()) ctx.warn("The HWP document contains no readable text.");
  return AnyIR(std::move(doc));
}

// --------------------------------------------------------------------------- VSDX

Result<AnyIR> read_vsdx(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Package package, Package::open(ctx.input, ctx.limits));
  std::vector<std::pair<std::string, std::string>> pages;  // part, name
  if (auto list = package.xml("visio/pages/pages.xml", false)) {
    for (const auto& rel : package.relationships("visio/pages/pages.xml")) {
      for (XmlNode page : list->root().children_local("Page")) {
        if (attr(child(page, "Rel"), "id") == rel.id) pages.emplace_back(rel.target, std::string(attr(page, "Name")));
      }
    }
  }
  if (pages.empty()) return fail(Status::Decode, "the Visio drawing has no pages");
  ir::Document doc;
  for (const auto& [part, name] : pages) {
    auto xml = package.xml(part, true);
    if (!xml) continue;
    if (!doc.blocks.empty()) doc.blocks.push_back(ir::PageBreak{});
    if (!name.empty()) doc.blocks.push_back(ir::Heading{1, {ir::Text{name}}});
    for (XmlNode d : xml->root().descendants()) {
      if (!d.is_element() || d.local_name() != "Text") continue;
      std::string text(util::trim(d.text()));
      if (!text.empty()) doc.blocks.push_back(ir::Paragraph{text_with_breaks(text)});
    }
  }
  return AnyIR(std::move(doc));
}

void add_reader(Registry& registry, const char* id, ReadFn reader, std::vector<OptionSpec> options = {}) {
  Codec codec;
  codec.format_id = id;
  codec.kind = Kind::Document;
  codec.read = std::move(reader);
  codec.read_options = std::move(options);
  registry.add_codec(std::move(codec));
}

FormatInfo format(const char* id, const char* name, const char* description, Category category,
                  std::vector<std::string> extensions, const char* mime, int popularity) {
  FormatInfo info;
  info.id = id;
  info.name = name;
  info.description = description;
  info.category = category;
  info.extensions = std::move(extensions);
  info.mime = mime;
  info.popularity = popularity;
  return info;
}

}  // namespace

void register_misc_text(Registry& registry) {
  {
    FormatInfo info = format("abw", "AbiWord document", "AbiWord word-processing document.", Category::Document,
                             {"abw", "zabw"}, "application/x-abiword", 10);
    info.sniff = [](ByteView data, std::string_view filename) {
      const std::string_view head = as_chars(data).substr(0, 2048);
      if (head.find("<abiword") != std::string_view::npos) return 95;
      if (head.starts_with("\x1F\x8B") && util::iends_with(filename, ".zabw")) return 90;
      return 0;
    };
    registry.add_format(std::move(info));
    add_reader(registry, "abw", read_abw, document_read_options());
  }
  {
    FormatInfo info = format("hwpx", "Hangul HWPX document", "Hancom Office word-processing document (OWPML).",
                             Category::Document, {"hwpx"}, "application/hwp+zip", 10);
    info.sniff = [](ByteView data, std::string_view) {
      return leading_mimetype(data) == "application/hwp+zip" || zip_mentions(data, "Contents/section0.xml") ? 94 : 0;
    };
    registry.add_format(std::move(info));
    add_reader(registry, "hwpx", read_hwpx);
  }
  {
    FormatInfo info = format("hwp", "Hangul HWP document", "Hancom Office word-processing document (HWP 5).",
                             Category::Document, {"hwp"}, "application/x-hwp", 10);
    info.sniff = [](ByteView data, std::string_view) { return compound_file_has(data, u"FileHeader") && compound_file_has(data, u"BodyText") ? 95 : 0; };
    registry.add_format(std::move(info));
    add_reader(registry, "hwp", read_hwp);
  }
  {
    FormatInfo info = format("vsdx", "Visio drawing", "Microsoft Visio drawing (Office Open XML); its text is read.",
                             Category::Vector, {"vsdx"}, "application/vnd.ms-visio.drawing.main+xml", 15);
    info.sniff = [](ByteView data, std::string_view) { return zip_mentions(data, "visio/pages/") ? 95 : 0; };
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "vsdx";
    codec.kind = Kind::Document;
    codec.read = read_vsdx;
    codec.read_cost = 30;
    codec.lossy = true;
    registry.add_codec(std::move(codec));
  }
}

void register_ebook_wrappers(Registry& registry) {
  {
    FormatInfo info = format("fbz", "FictionBook (zipped)", "FictionBook e-book in a ZIP archive.", Category::Ebook,
                             {"fbz", "fb2.zip"}, "application/x-zip-compressed-fb2", 15);
    info.sniff = [](ByteView data, std::string_view filename) {
      if (!as_chars(data).starts_with("PK\x03\x04")) return 0;
      const bool name = util::iends_with(filename, ".fbz") || util::iends_with(filename, ".fb2.zip");
      return zip_mentions(data, ".fb2", 1024) ? (name ? 96 : 88) : 0;
    };
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "fbz";
    codec.kind = Kind::Document;
    codec.read = read_fbz;
    codec.write = write_fbz;
    registry.add_codec(std::move(codec));
  }
  {
    FormatInfo info = format("tcr", "TCR e-book", "Psion text-compressed e-book.", Category::Ebook, {"tcr"},
                             "application/x-tcr-ebook", 5);
    info.signatures = {Signature::at(0, "'!!8-Bit!!'")};
    registry.add_format(std::move(info));
    add_reader(registry, "tcr", read_tcr);
  }
}

}  // namespace cvt::office
